/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#ifndef OPENORIENTEERING_GDAL_OGC_CONNECTION_H
#define OPENORIENTEERING_GDAL_OGC_CONNECTION_H

#include <QByteArray>
#include <QString>

namespace OpenOrienteering {

struct GdalOgcConnection
{
	enum class ServiceType
	{
		Auto = 0,
		Wms,
		Wmts
	};

	enum class Authentication
	{
		None = 0,
		Basic,
		Bearer,
		ApiKey
	};

	enum class ApiKeyPlacement
	{
		Header = 0,
		Query
	};

	ServiceType service_type = ServiceType::Auto;
	QString name;
	QString service_url;
	Authentication authentication = Authentication::None;
	QString username;
	QString secret;
	QString api_key_name;
	ApiKeyPlacement api_key_placement = ApiKeyPlacement::Header;
	QString credential_id;

	bool isValid() const;
	bool needsSecret() const;
	QString effectiveSecret() const;

	QString serviceUrlForStorage() const;
	QString serviceUrlForRequest() const;
	QString datasetNameForRequest(const QString& stored_dataset_name) const;
	QString sanitizeDatasetName(const QString& request_dataset_name) const;
	QString sanitizeForDisplay(QString text) const;

	QString ensureCredentialId();
	bool storeSecretSecurely();

	static bool secureCredentialStoreAvailable();
	static QString loadSecretSecurely(const QString& credential_id);
};

class GdalOgcHttpGuard
{
public:
	explicit GdalOgcHttpGuard(const GdalOgcConnection& connection);
	~GdalOgcHttpGuard();

	GdalOgcHttpGuard(const GdalOgcHttpGuard&) = delete;
	GdalOgcHttpGuard& operator=(const GdalOgcHttpGuard&) = delete;

private:
	void setOption(const char* key, const QByteArray& value);
	void clearOptions();

	QByteArray cache_path;
	QByteArray ca_bundle_path;
	QByteArray effective_secret;
	QByteArray auth_value;
	QByteArray userpwd_value;
	QByteArray bearer_value;
	QByteArray headers_value;
};

/** Returns the packaged TLS CA bundle used by GDAL/libcurl, if available. */
QString gdalOgcBundledCaBundlePath();

QString gdalOgcServiceTypeName(GdalOgcConnection::ServiceType type);

}  // namespace OpenOrienteering

#endif