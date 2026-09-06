/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#include "gdal/gdal_ogc_connection.h"

#include <string>

#include <QtGlobal>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

#include <cpl_conv.h>
#include <gdal.h>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <wincred.h>
#endif

namespace OpenOrienteering {
namespace {

QString credentialTarget(const QString& credential_id)
{
	return QStringLiteral("OpenOrienteeringMapper/OGC/") + credential_id;
}

QString addOrRemoveQuerySecret(const QString& input,
                               const QString& key,
                               const QString& value,
                               bool add)
{
	QUrl url(input);
	if (!url.isValid() || key.isEmpty())
		return input;

	QUrlQuery query(url);
	query.removeAllQueryItems(key);
	if (add && !value.isEmpty())
		query.addQueryItem(key, value);
	url.setQuery(query);
	return url.toString(QUrl::FullyEncoded);
}

int wmtsOptionStart(const QString& text)
{
	int first = -1;
	for (const auto& marker : {
	         QStringLiteral(",layer="),
	         QStringLiteral(",tilematrixset="),
	         QStringLiteral(",tilematrix="),
	         QStringLiteral(",zoom_level="),
	         QStringLiteral(",style="),
	         QStringLiteral(",extendbeyonddateline=") })
	{
		const int pos = text.indexOf(marker, 0, Qt::CaseInsensitive);
		if (pos >= 0 && (first < 0 || pos < first))
			first = pos;
	}
	return first;
}

QString rewriteDatasetUrl(const QString& dataset_name,
                          const QString& key,
                          const QString& value,
                          bool add)
{
	if (key.isEmpty())
		return dataset_name;

	QString prefix;
	QString remainder = dataset_name;
	bool is_wmts = false;
	if (remainder.startsWith(QStringLiteral("WMTS:"), Qt::CaseInsensitive))
	{
		prefix = remainder.left(5);
		remainder.remove(0, 5);
		is_wmts = true;
	}
	else if (remainder.startsWith(QStringLiteral("WMS:"), Qt::CaseInsensitive))
	{
		prefix = remainder.left(4);
		remainder.remove(0, 4);
	}
	else
	{
		return addOrRemoveQuerySecret(dataset_name, key, value, add);
	}

	QString suffix;
	if (is_wmts)
	{
		const int option_pos = wmtsOptionStart(remainder);
		if (option_pos >= 0)
		{
			suffix = remainder.mid(option_pos);
			remainder = remainder.left(option_pos);
		}
	}

	return prefix + addOrRemoveQuerySecret(remainder, key, value, add) + suffix;
}

QByteArray cacheNamespace(const GdalOgcConnection& connection)
{
	QByteArray material = connection.serviceUrlForStorage().toUtf8();
	material += '|';
	material += QByteArray::number(static_cast<int>(connection.service_type));
	material += '|';
	material += QByteArray::number(static_cast<int>(connection.authentication));
	material += '|';
	material += connection.username.toUtf8();
	material += '|';
	material += connection.api_key_name.toUtf8();
	material += '|';
	material += connection.credential_id.toUtf8();
	if (connection.credential_id.isEmpty() && connection.needsSecret())
	{
		material += '|';
		material += QCryptographicHash::hash(connection.effectiveSecret().toUtf8(),
		                                   QCryptographicHash::Sha256).toHex();
	}
	return QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex();
}

}  // namespace

bool GdalOgcConnection::isValid() const
{
	const QUrl url(service_url);
	return url.isValid()
	       && !url.host().isEmpty()
	       && (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0
	           || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)
	       && (authentication != Authentication::ApiKey || !api_key_name.trimmed().isEmpty());
}

bool GdalOgcConnection::needsSecret() const
{
	return authentication != Authentication::None;
}

QString GdalOgcConnection::effectiveSecret() const
{
	if (!secret.isEmpty())
		return secret;
	if (!credential_id.isEmpty())
		return loadSecretSecurely(credential_id);
	return {};
}

QString GdalOgcConnection::serviceUrlForStorage() const
{
	if (authentication == Authentication::ApiKey
	    && api_key_placement == ApiKeyPlacement::Query)
	{
		return addOrRemoveQuerySecret(service_url, api_key_name, QString{}, false);
	}
	return service_url;
}

QString GdalOgcConnection::serviceUrlForRequest() const
{
	const QString stored_url = serviceUrlForStorage();
	if (authentication == Authentication::ApiKey
	    && api_key_placement == ApiKeyPlacement::Query)
	{
		return addOrRemoveQuerySecret(stored_url, api_key_name, effectiveSecret(), true);
	}
	return stored_url;
}

QString GdalOgcConnection::datasetNameForRequest(const QString& stored_dataset_name) const
{
	if (authentication == Authentication::ApiKey
	    && api_key_placement == ApiKeyPlacement::Query)
	{
		return rewriteDatasetUrl(stored_dataset_name, api_key_name, effectiveSecret(), true);
	}
	return stored_dataset_name;
}

QString GdalOgcConnection::sanitizeDatasetName(const QString& request_dataset_name) const
{
	if (authentication == Authentication::ApiKey
	    && api_key_placement == ApiKeyPlacement::Query)
	{
		return rewriteDatasetUrl(request_dataset_name, api_key_name, QString{}, false);
	}
	return request_dataset_name;
}

QString GdalOgcConnection::sanitizeForDisplay(QString text) const
{
	const QString value = effectiveSecret();
	if (!value.isEmpty())
	{
		text.replace(value, QStringLiteral("***"), Qt::CaseSensitive);
		const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(value));
		if (encoded != value)
			text.replace(encoded, QStringLiteral("***"), Qt::CaseSensitive);
	}
	return text;
}

QString GdalOgcConnection::ensureCredentialId()
{
	if (!credential_id.isEmpty())
		return credential_id;

	QByteArray material;
	material += serviceUrlForStorage().toUtf8();
	material += '|';
	material += QByteArray::number(static_cast<int>(authentication));
	material += '|';
	material += username.toUtf8();
	material += '|';
	material += api_key_name.toUtf8();

	credential_id = QString::fromLatin1(
	        QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
	return credential_id;
}

bool GdalOgcConnection::storeSecretSecurely()
{
	if (!needsSecret() || secret.isEmpty())
		return true;

#ifdef Q_OS_WIN
	const QString target = credentialTarget(ensureCredentialId());
	const std::wstring target_w = target.toStdWString();
	const std::wstring username_w = username.toStdWString();
	const QByteArray blob = secret.toUtf8();

	CREDENTIALW credential {};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = const_cast<LPWSTR>(target_w.c_str());
	credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(blob.constData()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = username_w.empty() ? nullptr : const_cast<LPWSTR>(username_w.c_str());
	return CredWriteW(&credential, 0) == TRUE;
#else
	return false;
#endif
}

bool GdalOgcConnection::secureCredentialStoreAvailable()
{
#ifdef Q_OS_WIN
	return true;
#else
	return false;
#endif
}

QString GdalOgcConnection::loadSecretSecurely(const QString& credential_id)
{
#ifdef Q_OS_WIN
	if (credential_id.isEmpty())
		return {};

	const std::wstring target_w = credentialTarget(credential_id).toStdWString();
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(target_w.c_str(), CRED_TYPE_GENERIC, 0, &credential))
		return {};

	const QByteArray blob(
	        reinterpret_cast<const char*>(credential->CredentialBlob),
	        static_cast<int>(credential->CredentialBlobSize));
	CredFree(credential);
	return QString::fromUtf8(blob);
#else
	Q_UNUSED(credential_id)
	return {};
#endif
}

GdalOgcHttpGuard::GdalOgcHttpGuard(const GdalOgcConnection& connection)
: effective_secret(connection.effectiveSecret().toUtf8())
{
	const QString cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
	                           + QStringLiteral("/gdal-ogc/")
	                           + QString::fromLatin1(cacheNamespace(connection));
	QDir().mkpath(cache_root);
	cache_path = cache_root.toUtf8();

	setOption("GDAL_DEFAULT_WMS_CACHE_PATH", cache_path);
	setOption("GDAL_ENABLE_WMS_CACHE", QByteArrayLiteral("YES"));
	setOption("GDAL_MAX_CONNECTIONS", QByteArrayLiteral("6"));
	setOption("GDAL_HTTP_CONNECTTIMEOUT", QByteArrayLiteral("8"));
	setOption("GDAL_HTTP_TIMEOUT", QByteArrayLiteral("25"));
	setOption("GDAL_HTTP_MAX_RETRY", QByteArrayLiteral("1"));
	setOption("GDAL_HTTP_RETRY_DELAY", QByteArrayLiteral("1"));
	setOption("GDAL_HTTP_USERAGENT", QByteArrayLiteral("OpenOrienteering Mapper OGC client"));
#ifdef Q_OS_WIN
	const QString ca_bundle = gdalOgcBundledCaBundlePath();
	if (!ca_bundle.isEmpty())
	{
		ca_bundle_path = ca_bundle.toLocal8Bit();
		setOption("CURL_CA_BUNDLE", ca_bundle_path);
		setOption("SSL_CERT_FILE", ca_bundle_path);
	}
	setOption("GDAL_HTTP_USE_CAPI_STORE", QByteArrayLiteral("YES"));
#endif

	switch (connection.authentication)
	{
	case GdalOgcConnection::Authentication::None:
		break;
	case GdalOgcConnection::Authentication::Basic:
		auth_value = QByteArrayLiteral("BASIC");
		userpwd_value = connection.username.toUtf8() + ':' + effective_secret;
		setOption("GDAL_HTTP_AUTH", auth_value);
		setOption("GDAL_HTTP_USERPWD", userpwd_value);
		break;
	case GdalOgcConnection::Authentication::Bearer:
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 9, 0)
		auth_value = QByteArrayLiteral("BEARER");
		bearer_value = effective_secret;
		setOption("GDAL_HTTP_AUTH", auth_value);
		setOption("GDAL_HTTP_BEARER", bearer_value);
#else
		headers_value = QByteArrayLiteral("Authorization: Bearer ") + effective_secret;
		setOption("GDAL_HTTP_HEADERS", headers_value);
#endif
		break;
	case GdalOgcConnection::Authentication::ApiKey:
		if (connection.api_key_placement == GdalOgcConnection::ApiKeyPlacement::Header)
		{
			headers_value = connection.api_key_name.toUtf8()
			                + QByteArrayLiteral(": ") + effective_secret;
			setOption("GDAL_HTTP_HEADERS", headers_value);
		}
		break;
	}
}

GdalOgcHttpGuard::~GdalOgcHttpGuard()
{
	clearOptions();
}

void GdalOgcHttpGuard::setOption(const char* key, const QByteArray& value)
{
	CPLSetThreadLocalConfigOption(key, value.constData());
}

void GdalOgcHttpGuard::clearOptions()
{
	for (const char* key : {
	         "GDAL_DEFAULT_WMS_CACHE_PATH",
	         "GDAL_ENABLE_WMS_CACHE",
	         "GDAL_MAX_CONNECTIONS",
	         "GDAL_HTTP_CONNECTTIMEOUT",
	         "GDAL_HTTP_TIMEOUT",
	         "GDAL_HTTP_MAX_RETRY",
	         "GDAL_HTTP_RETRY_DELAY",
	         "GDAL_HTTP_USERAGENT",
	         "CURL_CA_BUNDLE",
	         "SSL_CERT_FILE",
	         "GDAL_HTTP_USE_CAPI_STORE",
	         "GDAL_HTTP_AUTH",
	         "GDAL_HTTP_USERPWD",
	         "GDAL_HTTP_BEARER",
	         "GDAL_HTTP_HEADERS" })
	{
		CPLSetThreadLocalConfigOption(key, nullptr);
	}
}

QString gdalOgcBundledCaBundlePath()
{
#ifdef Q_OS_WIN
	const QString candidate = QDir(QCoreApplication::applicationDirPath())
	                          .filePath(QStringLiteral("certs/ca-bundle.crt"));
	const QFileInfo bundle(candidate);
	if (bundle.isFile() && bundle.isReadable())
		return QDir::toNativeSeparators(bundle.absoluteFilePath());
#endif
	return {};
}

QString gdalOgcServiceTypeName(GdalOgcConnection::ServiceType type)
{
	switch (type)
	{
	case GdalOgcConnection::ServiceType::Wms:
		return QStringLiteral("WMS");
	case GdalOgcConnection::ServiceType::Wmts:
		return QStringLiteral("WMTS");
	default:
		return QStringLiteral("Auto");
	}
}

}  // namespace OpenOrienteering
