/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#include "gdal/gdal_ogc_catalog.h"

#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <cpl_error.h>
#include <cpl_string.h>
#include <gdal.h>

#include "gdal/gdal_manager.h"

namespace OpenOrienteering {
namespace {

QString trCatalog(const char* source_text)
{
	return QCoreApplication::translate("OpenOrienteering::GdalOgcCatalog", source_text);
}

QString queryValueCaseInsensitive(const QUrlQuery& query, const QString& key)
{
	for (const auto& item : query.queryItems(QUrl::FullyDecoded))
	{
		if (item.first.compare(key, Qt::CaseInsensitive) == 0)
			return item.second;
	}
	return {};
}

QString optionValue(const QString& text, const QString& key)
{
	const QRegularExpression expression(
	        QStringLiteral("(?:^|,)%1=([^,]+)").arg(QRegularExpression::escape(key)),
	        QRegularExpression::CaseInsensitiveOption);
	const auto match = expression.match(text);
	return match.hasMatch() ? QUrl::fromPercentEncoding(match.captured(1).toUtf8()) : QString{};
}

QString friendlyError(GdalOgcConnection::ServiceType type, const QString& raw)
{
	const QString service = gdalOgcServiceTypeName(type);
	if (raw.isEmpty())
		return trCatalog("%1 service could not be opened.").arg(service);
	if (raw.contains(QStringLiteral("401")) || raw.contains(QStringLiteral("403"))
	    || raw.contains(QStringLiteral("authentication"), Qt::CaseInsensitive))
	{
		return trCatalog("%1 authentication was rejected: %2").arg(service, raw);
	}
	if (raw.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)
	    || raw.contains(QStringLiteral("timeout"), Qt::CaseInsensitive))
	{
		return trCatalog("%1 server did not respond before the timeout: %2").arg(service, raw);
	}
	if (raw.contains(QStringLiteral("SSL"), Qt::CaseInsensitive)
	    || raw.contains(QStringLiteral("certificate"), Qt::CaseInsensitive))
	{
		return trCatalog("%1 HTTPS/TLS connection failed: %2").arg(service, raw);
	}
	if (raw.contains(QStringLiteral("HTTP"), Qt::CaseInsensitive))
		return trCatalog("%1 server returned an HTTP error: %2").arg(service, raw);
	return trCatalog("%1 connection failed: %2").arg(service, raw);
}

QString sanitizedDescription(const GdalOgcConnection& connection, const char* description)
{
	QString result = description ? QString::fromUtf8(description) : QString{};
	if (connection.authentication == GdalOgcConnection::Authentication::ApiKey
	    && connection.api_key_placement == GdalOgcConnection::ApiKeyPlacement::Query)
	{
		const QString secret = connection.effectiveSecret();
		if (!secret.isEmpty())
			result.replace(secret, QStringLiteral("***"), Qt::CaseSensitive);
	}
	return result;
}

GdalOgcCatalogResult openCatalog(const GdalOgcConnection& connection,
                                 GdalOgcConnection::ServiceType type)
{
	GdalOgcCatalogResult result;
	result.detected_type = type;

	if (connection.needsSecret() && connection.effectiveSecret().isEmpty())
	{
		result.error = trCatalog("Authentication credentials are missing.");
		return result;
	}

	GdalManager();
	GdalOgcHttpGuard http_guard(connection);
	CPLErrorReset();
	const char* driver_name = type == GdalOgcConnection::ServiceType::Wmts ? "WMTS" : "WMS";
	if (!GDALGetDriverByName(driver_name))
	{
		result.error = trCatalog("The packaged GDAL %1 driver is unavailable.")
		               .arg(QString::fromLatin1(driver_name));
		return result;
	}

	const QString service_url = connection.serviceUrlForRequest();
	const QString open_name = (type == GdalOgcConnection::ServiceType::Wmts)
	                          ? QStringLiteral("WMTS:") + service_url
	                          : QStringLiteral("WMS:") + service_url;
	const QByteArray open_name_utf8 = open_name.toUtf8();
	const char* allowed_wmts[] = { "WMTS", nullptr };
	const char* allowed_wms[] = { "WMS", nullptr };
	const char* const* allowed = (type == GdalOgcConnection::ServiceType::Wmts)
	                             ? allowed_wmts : allowed_wms;

	GDALDatasetH dataset = GDALOpenEx(open_name_utf8.constData(),
	                                  GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR,
	                                  allowed, nullptr, nullptr);
	if (!dataset)
	{
		const QString raw = connection.sanitizeForDisplay(QString::fromUtf8(CPLGetLastErrorMsg()));
		result.error = friendlyError(type, raw);
		return result;
	}

	CSLConstList metadata = GDALGetMetadata(dataset, "SUBDATASETS");
	for (int i = 1; ; ++i)
	{
		const QByteArray name_key = QByteArrayLiteral("SUBDATASET_")
		                            + QByteArray::number(i)
		                            + QByteArrayLiteral("_NAME");
		const char* name = CSLFetchNameValue(metadata, name_key.constData());
		if (!name)
			break;

		const QByteArray desc_key = QByteArrayLiteral("SUBDATASET_")
		                            + QByteArray::number(i)
		                            + QByteArrayLiteral("_DESC");
		const char* desc = CSLFetchNameValue(metadata, desc_key.constData());

		result.sources.push_back(GdalOgcCatalog::describeSource(
		        connection,
		        QString::fromUtf8(name),
		        sanitizedDescription(connection, desc)));
	}

	if (result.sources.isEmpty() && GDALGetRasterCount(dataset) > 0)
	{
		const char* opened_driver_name = GDALGetDriverShortName(GDALGetDatasetDriver(dataset));
		auto source = GdalOgcCatalog::describeSource(
		        connection,
		        open_name,
		        QStringLiteral("%1 — %2")
		        .arg(gdalOgcServiceTypeName(type),
		             QString::fromLatin1(opened_driver_name ? opened_driver_name : "GDAL")));
		result.sources.push_back(std::move(source));
	}

	GDALClose(dataset);

	if (result.sources.isEmpty())
		result.error = trCatalog("The service contains no raster layers that GDAL can open.");

	return result;
}

}  // namespace

GdalOgcSource GdalOgcCatalog::describeSource(
	const GdalOgcConnection& connection,
	const QString& dataset_name,
	const QString& description)
{
	GdalOgcSource source;
	source.dataset_name = connection.sanitizeDatasetName(dataset_name);
	source.description = connection.sanitizeForDisplay(description);
	source.title = source.description.isEmpty() ? source.dataset_name : source.description;

	QString parameters = source.dataset_name;
	if (parameters.startsWith(QStringLiteral("WMS:"), Qt::CaseInsensitive))
		parameters.remove(0, 4);
	else if (parameters.startsWith(QStringLiteral("WMTS:"), Qt::CaseInsensitive))
		parameters.remove(0, 5);

	const int option_start = parameters.indexOf(QLatin1Char(','));
	const QString url_text = option_start >= 0 ? parameters.left(option_start) : parameters;
	const QUrlQuery query{QUrl{url_text}};
	source.layer_name = queryValueCaseInsensitive(query, QStringLiteral("LAYERS"));
	if (source.layer_name.isEmpty())
		source.layer_name = queryValueCaseInsensitive(query, QStringLiteral("LAYER"));
	source.crs = queryValueCaseInsensitive(query, QStringLiteral("CRS"));
	if (source.crs.isEmpty())
		source.crs = queryValueCaseInsensitive(query, QStringLiteral("SRS"));
	source.format = queryValueCaseInsensitive(query, QStringLiteral("FORMAT"));
	source.style = queryValueCaseInsensitive(query, QStringLiteral("STYLES"));

	if (source.layer_name.isEmpty())
		source.layer_name = optionValue(parameters, QStringLiteral("layer"));
	if (source.style.isEmpty())
		source.style = optionValue(parameters, QStringLiteral("style"));
	if (source.format.isEmpty())
		source.format = optionValue(parameters, QStringLiteral("format"));
	source.tile_matrix_set = optionValue(parameters, QStringLiteral("tilematrixset"));

	if (source.crs.isEmpty())
	{
		const QRegularExpression epsg(QStringLiteral("EPSG[:/](\\d+)"),
		                              QRegularExpression::CaseInsensitiveOption);
		const auto match = epsg.match(source.description + QLatin1Char(' ') + source.dataset_name);
		if (match.hasMatch())
			source.crs = QStringLiteral("EPSG:") + match.captured(1);
	}
	if (source.format.isEmpty())
		source.format = QStringLiteral("image/png");
	return source;
}

GdalOgcCatalogResult GdalOgcCatalog::discover(const GdalOgcConnection& connection)
{
	if (!connection.isValid())
	{
		GdalOgcCatalogResult result;
		result.error = trCatalog("Invalid WMS/WMTS service URL or authentication settings.");
		return result;
	}

	if (connection.service_type == GdalOgcConnection::ServiceType::Wms)
		return openCatalog(connection, GdalOgcConnection::ServiceType::Wms);
	if (connection.service_type == GdalOgcConnection::ServiceType::Wmts)
		return openCatalog(connection, GdalOgcConnection::ServiceType::Wmts);

	const bool wmts_hint = connection.service_url.contains(QStringLiteral("wmts"), Qt::CaseInsensitive);
	const auto first_type = wmts_hint ? GdalOgcConnection::ServiceType::Wmts
	                                  : GdalOgcConnection::ServiceType::Wms;
	const auto second_type = wmts_hint ? GdalOgcConnection::ServiceType::Wms
	                                   : GdalOgcConnection::ServiceType::Wmts;

	auto first = openCatalog(connection, first_type);
	if (first.ok())
		return first;

	auto second = openCatalog(connection, second_type);
	if (second.ok())
		return second;

	GdalOgcCatalogResult result;
	const QString wms_error = first_type == GdalOgcConnection::ServiceType::Wms
	                          ? first.error : second.error;
	const QString wmts_error = first_type == GdalOgcConnection::ServiceType::Wmts
	                           ? first.error : second.error;
	result.error = trCatalog("WMS: %1\nWMTS: %2").arg(wms_error, wmts_error);
	return result;
}

}  // namespace OpenOrienteering
