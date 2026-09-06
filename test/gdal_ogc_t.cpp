/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#include "gdal_ogc_t.h"

#include <QtTest>
#include <QFileInfo>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "core/map.h"
#include "core/map_view.h"
#include "gdal/gdal_ogc_catalog.h"
#include "gdal/gdal_ogc_connection.h"
#include "gdal/gdal_online_raster_template.h"

#include <cpl_conv.h>

namespace OpenOrienteering {

void GdalOgcTest::validatesHttpUrls()
{
	GdalOgcConnection connection;
	connection.service_url = QStringLiteral("https://example.test/wms?");
	QVERIFY(connection.isValid());

	connection.service_url = QStringLiteral("file:///tmp/service.xml");
	QVERIFY(!connection.isValid());

	connection.service_url = QStringLiteral("not a URL");
	QVERIFY(!connection.isValid());
}

void GdalOgcTest::protectsQueryApiKeys()
{
	GdalOgcConnection connection;
	connection.service_url = QStringLiteral(
	        "https://example.test/wms?token=old-secret&foo=1");
	connection.authentication = GdalOgcConnection::Authentication::ApiKey;
	connection.api_key_placement = GdalOgcConnection::ApiKeyPlacement::Query;
	connection.api_key_name = QStringLiteral("token");
	connection.secret = QStringLiteral("new secret/+token");

	const QString stored_url = connection.serviceUrlForStorage();
	QVERIFY(!stored_url.contains(QStringLiteral("old-secret")));
	QVERIFY(!stored_url.contains(QStringLiteral("new secret/+token")));
	QVERIFY(stored_url.contains(QStringLiteral("foo=1")));

	const QString request_url = connection.serviceUrlForRequest();
	QCOMPARE(QUrlQuery(QUrl(request_url)).queryItemValue(
	                 QStringLiteral("token"), QUrl::FullyDecoded),
	         QStringLiteral("new secret/+token"));

	const QString stored_dataset = QStringLiteral(
	        "WMTS:https://example.test/wmts?foo=1&token=new%20secret%2F%2Btoken,layer=test,tilematrixset=web");
	const QString sanitized = connection.sanitizeDatasetName(stored_dataset);
	QVERIFY(!sanitized.contains(QStringLiteral("new%20secret%2F%2Btoken")));
	QVERIFY(sanitized.contains(QStringLiteral(",layer=test,tilematrixset=web")));
	QCOMPARE(connection.sanitizeForDisplay(QStringLiteral("failed: new%20secret%2F%2Btoken")),
	         QStringLiteral("failed: ***"));
}

void GdalOgcTest::describesWmsSources()
{
	GdalOgcConnection connection;
	const auto source = GdalOgcCatalog::describeSource(
	        connection,
	        QStringLiteral("WMS:https://example.test/wms?LAYERS=roads&CRS=EPSG%3A3059&FORMAT=image%2Fpng&STYLES=default"),
	        QStringLiteral("Road network"));

	QCOMPARE(source.title, QStringLiteral("Road network"));
	QCOMPARE(source.layer_name, QStringLiteral("roads"));
	QCOMPARE(source.crs, QStringLiteral("EPSG:3059"));
	QCOMPARE(source.format, QStringLiteral("image/png"));
	QCOMPARE(source.style, QStringLiteral("default"));
}

void GdalOgcTest::describesWmtsSources()
{
	GdalOgcConnection connection;
	const auto source = GdalOgcCatalog::describeSource(
	        connection,
	        QStringLiteral("WMTS:https://example.test/WMTSCapabilities.xml,layer=BlueMarble,"
	                       "tilematrixset=GoogleMapsCompatible_Level9,style=default"),
	        QStringLiteral("Blue Marble EPSG:3857"));

	QCOMPARE(source.layer_name, QStringLiteral("BlueMarble"));
	QCOMPARE(source.crs, QStringLiteral("EPSG:3857"));
	QCOMPARE(source.style, QStringLiteral("default"));
	QCOMPARE(source.tile_matrix_set, QStringLiteral("GoogleMapsCompatible_Level9"));
	QCOMPARE(source.format, QStringLiteral("image/png"));
}

void GdalOgcTest::usesPackagedCaBundleOnWindows()
{
#ifdef Q_OS_WIN
	const QString ca_bundle = gdalOgcBundledCaBundlePath();
	if (ca_bundle.isEmpty())
		QSKIP("No packaged TLS CA bundle in this build-tree test runtime");
	QVERIFY(QFileInfo(ca_bundle).isReadable());

	GdalOgcConnection connection;
	{
		GdalOgcHttpGuard guard(connection);
		QCOMPARE(QString::fromLocal8Bit(
		                 CPLGetThreadLocalConfigOption("CURL_CA_BUNDLE", "")),
		         ca_bundle);
		QCOMPARE(QString::fromLocal8Bit(
		                 CPLGetThreadLocalConfigOption("SSL_CERT_FILE", "")),
		         ca_bundle);
		QCOMPARE(QString::fromLatin1(
		                 CPLGetThreadLocalConfigOption("GDAL_HTTP_USE_CAPI_STORE", "")),
		         QStringLiteral("YES"));
	}
#else
	QSKIP("The packaged CA-bundle lookup is specific to standalone Windows builds");
#endif
}

void GdalOgcTest::supportsRegionalOverviewZoom()
{
	QVERIFY(MapView::zoom_out_limit <= 1.0 / 4096.0);
	Map map;
	MapView view(&map);
	view.setZoom(1.0 / 2048.0);
	QVERIFY(qFuzzyCompare(view.getZoom(), 1.0 / 2048.0));
}

void GdalOgcTest::preservesProjectConfigurationWithoutSecrets()
{
	Map map;
	GdalOnlineRasterTemplate online_template(QStringLiteral("unused"), &map);
	QXmlStreamReader reader(QStringLiteral(
	        "<online-raster "
	        "dataset=\"WMTS:https://example.test/capabilities.xml,layer=BlueMarble,"
	        "tilematrixset=WebMercatorQuad,style=default\" "
	        "display-name=\"Blue Marble\" connection-name=\"Example WMTS\" "
	        "service-url=\"https://example.test/capabilities.xml\" "
	        "service-type=\"2\" authentication=\"3\" username=\"\" "
	        "api-key-name=\"token\" api-key-placement=\"1\" "
	        "credential-id=\"opaque-credential-id\"/>"));
	QVERIFY(reader.readNextStartElement());
	QVERIFY(online_template.loadTypeSpecificTemplateConfiguration(reader));
	QVERIFY(online_template.finishTypeSpecificTemplateConfiguration());

	QString saved;
	QXmlStreamWriter writer(&saved);
	online_template.saveTypeSpecificTemplateConfiguration(writer);
	QVERIFY(saved.contains(QStringLiteral("connection-name=\"Example WMTS\"")));
	QVERIFY(saved.contains(QStringLiteral("layer=BlueMarble")));
	QVERIFY(saved.contains(QStringLiteral("tilematrixset=WebMercatorQuad")));
	QVERIFY(saved.contains(QStringLiteral("credential-id=\"opaque-credential-id\"")));
	QVERIFY(!saved.contains(QStringLiteral("secret"), Qt::CaseInsensitive));
	QVERIFY(!saved.contains(QStringLiteral("password"), Qt::CaseInsensitive));
}

}  // namespace OpenOrienteering

#ifndef Q_OS_MACOS
namespace {
auto Q_DECL_UNUSED qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");
}
#endif

QTEST_MAIN(OpenOrienteering::GdalOgcTest)