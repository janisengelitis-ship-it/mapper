/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#include "gdal/gdal_ogc_dialog.h"

#include <functional>
#include <algorithm>
#include <utility>

#include <QCheckBox>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace OpenOrienteering {
namespace {

class CatalogTask final : public QRunnable
{
public:
	CatalogTask(GdalOgcConnection connection,
	            std::function<void(GdalOgcCatalogResult)> completion)
	: connection(std::move(connection))
	, completion(std::move(completion))
	{}

	void run() override
	{
		completion(GdalOgcCatalog::discover(connection));
	}

private:
	GdalOgcConnection connection;
	std::function<void(GdalOgcCatalogResult)> completion;
};

class PrepareSelectionTask final : public QRunnable
{
public:
	PrepareSelectionTask(GdalOgcConnection connection,
	                     QString dataset_name,
	                     QString map_crs_spec,
	                     std::function<void(GdalOnlinePreparedDataPtr, QString)> completion)
	: connection(std::move(connection))
	, dataset_name(std::move(dataset_name))
	, map_crs_spec(std::move(map_crs_spec))
	, completion(std::move(completion))
	{}

	void run() override
	{
		QString error;
		auto prepared = GdalOnlineRasterTemplate::prepareDataset(
		        connection, dataset_name, map_crs_spec, error);
		completion(std::move(prepared), std::move(error));
	}

private:
	GdalOgcConnection connection;
	QString dataset_name;
	QString map_crs_spec;
	std::function<void(GdalOnlinePreparedDataPtr, QString)> completion;
};

const auto connections_array = QStringLiteral("GDAL/online-raster-connections");

void writeConnection(QSettings& settings, const GdalOgcConnection& connection)
{
	settings.setValue(QStringLiteral("name"), connection.name);
	settings.setValue(QStringLiteral("url"), connection.serviceUrlForStorage());
	settings.setValue(QStringLiteral("service-type"), static_cast<int>(connection.service_type));
	settings.setValue(QStringLiteral("authentication"), static_cast<int>(connection.authentication));
	settings.setValue(QStringLiteral("username"), connection.username);
	settings.setValue(QStringLiteral("api-key-name"), connection.api_key_name);
	settings.setValue(QStringLiteral("api-key-placement"), static_cast<int>(connection.api_key_placement));
	settings.setValue(QStringLiteral("credential-id"), connection.credential_id);
}

GdalOgcConnection readConnection(QSettings& settings)
{
	GdalOgcConnection connection;
	connection.name = settings.value(QStringLiteral("name")).toString();
	connection.service_url = settings.value(QStringLiteral("url")).toString();
	connection.service_type = static_cast<GdalOgcConnection::ServiceType>(
	        settings.value(QStringLiteral("service-type")).toInt());
	connection.authentication = static_cast<GdalOgcConnection::Authentication>(
	        settings.value(QStringLiteral("authentication")).toInt());
	connection.username = settings.value(QStringLiteral("username")).toString();
	connection.api_key_name = settings.value(QStringLiteral("api-key-name")).toString();
	connection.api_key_placement = static_cast<GdalOgcConnection::ApiKeyPlacement>(
	        settings.value(QStringLiteral("api-key-placement")).toInt());
	connection.credential_id = settings.value(QStringLiteral("credential-id")).toString();
	return connection;
}

QWidget* noAuthenticationPage(QWidget* parent)
{
	auto* page = new QWidget(parent);
	auto* layout = new QHBoxLayout(page);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(new QLabel(GdalOgcDialog::tr("No credentials required."), page));
	layout->addStretch();
	return page;
}

}  // namespace

GdalOgcDialog::GdalOgcDialog(const QString& preferred_crs, QWidget* parent)
: QDialog(parent)
, preferred_crs(preferred_crs)
{
	setWindowTitle(tr("Add WMS/WMTS background map"));
	resize(900, 600);

	saved_connection_box = new QComboBox(this);
	saved_connection_box->addItem(tr("New connection..."), -1);

	name_edit = new QLineEdit(this);
	name_edit->setPlaceholderText(tr("Example: Regional orthophoto service"));

	service_type_box = new QComboBox(this);
	service_type_box->addItem(tr("Auto detect"), static_cast<int>(GdalOgcConnection::ServiceType::Auto));
	service_type_box->addItem(QStringLiteral("WMS"), static_cast<int>(GdalOgcConnection::ServiceType::Wms));
	service_type_box->addItem(QStringLiteral("WMTS"), static_cast<int>(GdalOgcConnection::ServiceType::Wmts));

	url_edit = new QLineEdit(this);
	url_edit->setPlaceholderText(tr("https://server.example/ogc/service"));

	authentication_box = new QComboBox(this);
	authentication_box->addItem(tr("None"), static_cast<int>(GdalOgcConnection::Authentication::None));
	authentication_box->addItem(tr("Username / password"), static_cast<int>(GdalOgcConnection::Authentication::Basic));
	authentication_box->addItem(tr("Bearer token"), static_cast<int>(GdalOgcConnection::Authentication::Bearer));
	authentication_box->addItem(tr("API key"), static_cast<int>(GdalOgcConnection::Authentication::ApiKey));

	authentication_stack = new QStackedWidget(this);
	authentication_stack->addWidget(noAuthenticationPage(authentication_stack));

	{
		auto* page = new QWidget(authentication_stack);
		auto* form = new QFormLayout(page);
		form->setContentsMargins(0, 0, 0, 0);
		username_edit = new QLineEdit(page);
		password_edit = new QLineEdit(page);
		password_edit->setEchoMode(QLineEdit::Password);
		form->addRow(tr("Username:"), username_edit);
		form->addRow(tr("Password:"), password_edit);
		authentication_stack->addWidget(page);
	}

	{
		auto* page = new QWidget(authentication_stack);
		auto* form = new QFormLayout(page);
		form->setContentsMargins(0, 0, 0, 0);
		bearer_edit = new QLineEdit(page);
		bearer_edit->setEchoMode(QLineEdit::Password);
		form->addRow(tr("Token:"), bearer_edit);
		authentication_stack->addWidget(page);
	}

	{
		auto* page = new QWidget(authentication_stack);
		auto* form = new QFormLayout(page);
		form->setContentsMargins(0, 0, 0, 0);
		api_key_name_edit = new QLineEdit(page);
		api_key_name_edit->setPlaceholderText(QStringLiteral("X-API-Key"));
		api_key_value_edit = new QLineEdit(page);
		api_key_value_edit->setEchoMode(QLineEdit::Password);
		api_key_placement_box = new QComboBox(page);
		api_key_placement_box->addItem(tr("HTTP header"), static_cast<int>(GdalOgcConnection::ApiKeyPlacement::Header));
		api_key_placement_box->addItem(tr("URL query parameter"), static_cast<int>(GdalOgcConnection::ApiKeyPlacement::Query));
		form->addRow(tr("Key name:"), api_key_name_edit);
		form->addRow(tr("Key value:"), api_key_value_edit);
		form->addRow(tr("Send as:"), api_key_placement_box);
		authentication_stack->addWidget(page);
	}

	remember_credentials_box = new QCheckBox(tr("Remember credentials securely on this computer"), this);
	remember_credentials_box->setChecked(GdalOgcConnection::secureCredentialStoreAvailable());
	remember_credentials_box->setEnabled(GdalOgcConnection::secureCredentialStoreAvailable());

	connect_button = new QPushButton(tr("Connect"), this);
	status_label = new QLabel(tr("Not connected"), this);
	status_label->setWordWrap(true);

	source_tree = new QTreeWidget(this);
	source_tree->setEnabled(false);
	source_tree->setRootIsDecorated(false);
	source_tree->setUniformRowHeights(true);
	source_tree->setSelectionMode(QAbstractItemView::SingleSelection);
	source_tree->setHeaderLabels({ tr("Title"), tr("Name"), tr("CRS / Tile matrix set"),
	                               tr("Format"), tr("Style") });
	source_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int column = 1; column < 5; ++column)
		source_tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
	source_tree->setMinimumHeight(190);

	auto* form = new QFormLayout();
	form->addRow(tr("Saved connection:"), saved_connection_box);
	form->addRow(tr("Name:"), name_edit);
	form->addRow(tr("Service type:"), service_type_box);
	form->addRow(tr("Service URL:"), url_edit);
	form->addRow(tr("Authentication:"), authentication_box);
	form->addRow(tr("Credentials:"), authentication_stack);
	form->addRow(QString{}, remember_credentials_box);
	form->addRow(QString{}, connect_button);
	form->addRow(tr("Status:"), status_label);
	form->addRow(tr("Layer:"), source_tree);

	button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	button_box->button(QDialogButtonBox::Ok)->setText(tr("Add"));
	button_box->button(QDialogButtonBox::Ok)->setEnabled(false);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addStretch();
	layout->addWidget(button_box);

	connect(saved_connection_box, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &GdalOgcDialog::savedConnectionChanged);
	connect(authentication_box, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &GdalOgcDialog::authenticationChanged);
	connect(service_type_box, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [this](int) { resetCredentialReference(); clearSources(); });
	connect(name_edit, &QLineEdit::textEdited, this, [this](const QString&) { clearSources(); });
	connect(url_edit, &QLineEdit::textEdited, this, [this](const QString&) {
		resetCredentialReference();
		clearSources();
	});
	for (auto* edit : { username_edit, password_edit, bearer_edit, api_key_name_edit, api_key_value_edit })
		connect(edit, &QLineEdit::textEdited, this, [this](const QString&) {
			resetCredentialReference();
			clearSources();
		});
	connect(api_key_placement_box, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [this](int) { resetCredentialReference(); clearSources(); });
	connect(remember_credentials_box, &QCheckBox::toggled, this, [this](bool checked) {
		if (checked || form_credential_id.isEmpty() || populating_form)
			return;
		const QString secret = GdalOgcConnection::loadSecretSecurely(form_credential_id);
		const auto authentication = static_cast<GdalOgcConnection::Authentication>(
		        authentication_box->currentData().toInt());
		if (authentication == GdalOgcConnection::Authentication::Basic)
			password_edit->setText(secret);
		else if (authentication == GdalOgcConnection::Authentication::Bearer)
			bearer_edit->setText(secret);
		else if (authentication == GdalOgcConnection::Authentication::ApiKey)
			api_key_value_edit->setText(secret);
		resetCredentialReference();
	});
	connect(connect_button, &QPushButton::clicked, this, [this]() {
		if (busy)
			cancelRequest();
		else
			testConnection();
	});
	connect(button_box, &QDialogButtonBox::accepted, this, &GdalOgcDialog::acceptSelection);
	connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

	authenticationChanged(authentication_box->currentIndex());
	loadSavedConnections();
}

void GdalOgcDialog::authenticationChanged(int index)
{
	authentication_stack->setCurrentIndex(index);
	const auto auth = static_cast<GdalOgcConnection::Authentication>(authentication_box->currentData().toInt());
	remember_credentials_box->setEnabled(auth != GdalOgcConnection::Authentication::None
	                                      && GdalOgcConnection::secureCredentialStoreAvailable());
	if (auth == GdalOgcConnection::Authentication::None)
		remember_credentials_box->setChecked(false);
	else if (GdalOgcConnection::secureCredentialStoreAvailable())
		remember_credentials_box->setChecked(true);
	resetCredentialReference();
	clearSources();
}

GdalOgcConnection GdalOgcDialog::connectionFromForm() const
{
	GdalOgcConnection connection;
	connection.name = name_edit->text().trimmed();
	connection.service_type = static_cast<GdalOgcConnection::ServiceType>(service_type_box->currentData().toInt());
	connection.service_url = QUrl::fromUserInput(url_edit->text().trimmed()).toString(QUrl::FullyEncoded);
	connection.authentication = static_cast<GdalOgcConnection::Authentication>(authentication_box->currentData().toInt());
	connection.credential_id = form_credential_id;

	switch (connection.authentication)
	{
	case GdalOgcConnection::Authentication::None:
		break;
	case GdalOgcConnection::Authentication::Basic:
		connection.username = username_edit->text();
		connection.secret = password_edit->text();
		break;
	case GdalOgcConnection::Authentication::Bearer:
		connection.secret = bearer_edit->text();
		break;
	case GdalOgcConnection::Authentication::ApiKey:
		connection.api_key_name = api_key_name_edit->text().trimmed();
		connection.secret = api_key_value_edit->text();
		connection.api_key_placement = static_cast<GdalOgcConnection::ApiKeyPlacement>(api_key_placement_box->currentData().toInt());
		break;
	}
	connection.service_url = connection.serviceUrlForStorage();
	return connection;
}

void GdalOgcDialog::testConnection()
{
	const GdalOgcConnection connection = connectionFromForm();
	clearSources();
	if (connection.name.isEmpty())
	{
		status_label->setText(tr("Enter a connection name first."));
		return;
	}
	if (!connection.isValid())
	{
		status_label->setText(tr("Invalid service URL or authentication settings."));
		return;
	}
	if (connection.needsSecret() && connection.effectiveSecret().isEmpty())
	{
		status_label->setText(tr("Enter the authentication secret first."));
		return;
	}

	setBusy(true);
	status_label->setText(tr("Connecting and reading service capabilities..."));
	const quint64 serial = ++request_serial;

	QPointer<GdalOgcDialog> guard(this);
	auto completion = [guard, connection, serial](GdalOgcCatalogResult result) mutable {
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(), [guard, connection, serial, result = std::move(result)]() mutable {
			if (guard)
				guard->finishDiscovery(serial, connection, std::move(result));
		}, Qt::QueuedConnection);
	};

	auto* task = new CatalogTask(connection, std::move(completion));
	task->setAutoDelete(true);
	QThreadPool::globalInstance()->start(task);
}

void GdalOgcDialog::cancelRequest()
{
	++request_serial;
	preparing_layer = false;
	setBusy(false);
	status_label->setText(tr("Request cancelled."));
}

void GdalOgcDialog::finishDiscovery(quint64 serial,
                                    const GdalOgcConnection& connection,
                                    GdalOgcCatalogResult result)
{
	if (serial != request_serial)
		return;
	setBusy(false);
	if (!result.ok())
	{
		status_label->setText(result.error.isEmpty()
		                      ? tr("The service could not be opened.")
		                      : result.error);
		return;
	}

	active_connection = connection;
	active_connection.service_type = result.detected_type;
	active_sources = std::move(result.sources);

	QString preferred_authority = preferred_crs;
	const QRegularExpression epsg_expression(QStringLiteral("EPSG[:/](\\d+)"),
	                                         QRegularExpression::CaseInsensitiveOption);
	const auto preferred_match = epsg_expression.match(preferred_crs);
	if (preferred_match.hasMatch())
		preferred_authority = QStringLiteral("EPSG:") + preferred_match.captured(1);
	auto preferred = [&preferred_authority](const GdalOgcSource& source) {
		return !preferred_authority.isEmpty()
		       && (source.crs.compare(preferred_authority, Qt::CaseInsensitive) == 0
		           || source.dataset_name.contains(preferred_authority, Qt::CaseInsensitive));
	};
	std::stable_sort(active_sources.begin(), active_sources.end(),
	                 [&preferred](const GdalOgcSource& a, const GdalOgcSource& b) {
		return preferred(a) && !preferred(b);
	});

	for (int index = 0; index < active_sources.size(); ++index)
	{
		const auto& source = active_sources.at(index);
		QString crs = source.crs;
		if (!source.tile_matrix_set.isEmpty())
			crs += (crs.isEmpty() ? QString{} : QStringLiteral(" / ")) + source.tile_matrix_set;
		auto* item = new QTreeWidgetItem(source_tree,
		        { source.title, source.layer_name, crs, source.format, source.style });
		item->setData(0, Qt::UserRole, index);
		item->setToolTip(0, source.description);
	}
	source_tree->setEnabled(true);
	if (source_tree->topLevelItemCount() > 0)
		source_tree->setCurrentItem(source_tree->topLevelItem(0));
	button_box->button(QDialogButtonBox::Ok)->setEnabled(true);
	connect_button->setText(tr("Refresh"));
	status_label->setText(tr("Connected. %1 source(s) available via %2.")
	                      .arg(active_sources.size())
	                      .arg(gdalOgcServiceTypeName(active_connection.service_type)));
}

void GdalOgcDialog::acceptSelection()
{
	const auto* item = source_tree->currentItem();
	const int index = item ? item->data(0, Qt::UserRole).toInt() : -1;
	if (index < 0 || index >= active_sources.size())
		return;

	selected.connection = active_connection;
	selected.source = active_sources.at(index);
	selected.prepared_data.reset();
	preparing_layer = true;
	setBusy(true);
	status_label->setText(tr("Opening the selected layer and verifying map imagery..."));
	const quint64 serial = ++request_serial;

	QPointer<GdalOgcDialog> guard(this);
	auto completion = [guard, serial](GdalOnlinePreparedDataPtr prepared, QString error) mutable {
		if (!guard)
			return;
		QMetaObject::invokeMethod(guard.data(),
		                          [guard, serial, prepared = std::move(prepared),
		                           error = std::move(error)]() mutable {
			if (guard)
				guard->finishPreparation(serial, std::move(prepared), error);
		},
		                          Qt::QueuedConnection);
	};
	auto* task = new PrepareSelectionTask(selected.connection,
	                                      selected.source.dataset_name,
	                                      preferred_crs,
	                                      std::move(completion));
	task->setAutoDelete(true);
	QThreadPool::globalInstance()->start(task);
}

void GdalOgcDialog::finishPreparation(
	quint64 serial,
	GdalOnlinePreparedDataPtr prepared_data,
	const QString& error)
{
	if (serial != request_serial)
		return;
	preparing_layer = false;
	setBusy(false);
	if (!prepared_data)
	{
		status_label->setText(error.isEmpty()
		                      ? tr("The selected layer could not be opened.")
		                      : selected.connection.sanitizeForDisplay(error));
		return;
	}
	selected.prepared_data = std::move(prepared_data);

	if (remember_credentials_box->isChecked() && selected.connection.needsSecret())
	{
		if (!selected.connection.storeSecretSecurely())
		{
			QMessageBox::warning(this,
			                     tr("Credential storage"),
			                     tr("Windows Credential Manager could not store these credentials. "
			                        "They will be used only in the current Mapper session."));
			selected.connection.credential_id.clear();
		}
		else
		{
			selected.connection.secret.clear();
			form_credential_id = selected.connection.credential_id;
		}
	}

	saveConnectionDefinition(selected.connection);
	accept();
}

void GdalOgcDialog::setBusy(bool busy_value)
{
	busy = busy_value;
	saved_connection_box->setEnabled(!busy);
	name_edit->setEnabled(!busy);
	service_type_box->setEnabled(!busy);
	url_edit->setEnabled(!busy);
	authentication_box->setEnabled(!busy);
	authentication_stack->setEnabled(!busy);
	connect_button->setEnabled(true);
	connect_button->setText(busy ? tr("Cancel request")
	                             : (active_sources.isEmpty() ? tr("Connect") : tr("Refresh")));
	button_box->button(QDialogButtonBox::Ok)->setEnabled(!busy && !active_sources.isEmpty());
}

void GdalOgcDialog::clearSources()
{
	active_sources.clear();
	active_connection = {};
	source_tree->clear();
	source_tree->setEnabled(false);
	button_box->button(QDialogButtonBox::Ok)->setEnabled(false);
	connect_button->setText(tr("Connect"));
	if (!busy)
		status_label->setText(tr("Not connected"));
}

void GdalOgcDialog::loadSavedConnections()
{
	QSettings settings;
	const int count = settings.beginReadArray(connections_array);
	for (int index = 0; index < count; ++index)
	{
		settings.setArrayIndex(index);
		auto connection = readConnection(settings);
		if (!connection.name.isEmpty() && connection.isValid())
			saved_connections.push_back(std::move(connection));
	}
	settings.endArray();
	std::sort(saved_connections.begin(), saved_connections.end(),
	          [](const GdalOgcConnection& a, const GdalOgcConnection& b) {
		return a.name.localeAwareCompare(b.name) < 0;
	});
	for (int index = 0; index < saved_connections.size(); ++index)
		saved_connection_box->addItem(saved_connections.at(index).name, index);
}

void GdalOgcDialog::savedConnectionChanged(int)
{
	const int index = saved_connection_box->currentData().toInt();
	if (index < 0 || index >= saved_connections.size())
	{
		form_credential_id.clear();
		return;
	}
	populateSavedConnection(saved_connections.at(index));
}

void GdalOgcDialog::populateSavedConnection(const GdalOgcConnection& connection)
{
	populating_form = true;
	name_edit->setText(connection.name);
	url_edit->setText(connection.service_url);
	service_type_box->setCurrentIndex(service_type_box->findData(static_cast<int>(connection.service_type)));
	authentication_box->setCurrentIndex(authentication_box->findData(static_cast<int>(connection.authentication)));
	username_edit->setText(connection.username);
	password_edit->clear();
	bearer_edit->clear();
	api_key_name_edit->setText(connection.api_key_name);
	api_key_value_edit->clear();
	api_key_placement_box->setCurrentIndex(
	        api_key_placement_box->findData(static_cast<int>(connection.api_key_placement)));
	form_credential_id = connection.credential_id;
	remember_credentials_box->setChecked(!form_credential_id.isEmpty());
	populating_form = false;
	clearSources();
}

void GdalOgcDialog::saveConnectionDefinition(const GdalOgcConnection& connection)
{
	if (connection.name.isEmpty())
		return;

	int replace_index = -1;
	for (int index = 0; index < saved_connections.size(); ++index)
	{
		if (saved_connections.at(index).name.compare(connection.name, Qt::CaseInsensitive) == 0)
		{
			replace_index = index;
			break;
		}
	}
	if (replace_index >= 0)
		saved_connections[replace_index] = connection;
	else
		saved_connections.push_back(connection);

	QSettings settings;
	settings.remove(connections_array);
	settings.beginWriteArray(connections_array);
	for (int index = 0; index < saved_connections.size(); ++index)
	{
		settings.setArrayIndex(index);
		writeConnection(settings, saved_connections.at(index));
	}
	settings.endArray();
	settings.sync();
}

void GdalOgcDialog::resetCredentialReference()
{
	if (!populating_form)
		form_credential_id.clear();
}

}  // namespace OpenOrienteering
