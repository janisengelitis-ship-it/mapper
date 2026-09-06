/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#ifndef OPENORIENTEERING_GDAL_OGC_DIALOG_H
#define OPENORIENTEERING_GDAL_OGC_DIALOG_H

#include <QDialog>

#include "gdal/gdal_ogc_catalog.h"
#include "gdal/gdal_online_raster_template.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;

namespace OpenOrienteering {

struct GdalOgcSelection
{
	GdalOgcConnection connection;
	GdalOgcSource source;
	GdalOnlinePreparedDataPtr prepared_data;
};

class GdalOgcDialog final : public QDialog
{
	Q_OBJECT

public:
	explicit GdalOgcDialog(const QString& preferred_crs = QString{},
	                       QWidget* parent = nullptr);

	GdalOgcSelection selection() const { return selected; }

private slots:
	void authenticationChanged(int index);
	void testConnection();
	void cancelRequest();
	void acceptSelection();
	void savedConnectionChanged(int index);

private:
	GdalOgcConnection connectionFromForm() const;
	void setBusy(bool busy);
	void finishDiscovery(quint64 serial,
	                     const GdalOgcConnection& connection,
	                     GdalOgcCatalogResult result);
	void finishPreparation(quint64 serial,
	                       GdalOnlinePreparedDataPtr prepared_data,
	                       const QString& error);
	void clearSources();
	void loadSavedConnections();
	void populateSavedConnection(const GdalOgcConnection& connection);
	void saveConnectionDefinition(const GdalOgcConnection& connection);
	void resetCredentialReference();

	QComboBox* saved_connection_box = nullptr;
	QLineEdit* name_edit = nullptr;
	QComboBox* service_type_box = nullptr;
	QLineEdit* url_edit = nullptr;
	QComboBox* authentication_box = nullptr;
	QStackedWidget* authentication_stack = nullptr;
	QLineEdit* username_edit = nullptr;
	QLineEdit* password_edit = nullptr;
	QLineEdit* bearer_edit = nullptr;
	QLineEdit* api_key_name_edit = nullptr;
	QLineEdit* api_key_value_edit = nullptr;
	QComboBox* api_key_placement_box = nullptr;
	QCheckBox* remember_credentials_box = nullptr;
	QPushButton* connect_button = nullptr;
	QLabel* status_label = nullptr;
	QTreeWidget* source_tree = nullptr;
	QDialogButtonBox* button_box = nullptr;

	GdalOgcConnection active_connection;
	QVector<GdalOgcSource> active_sources;
	QVector<GdalOgcConnection> saved_connections;
	GdalOgcSelection selected;
	QString preferred_crs;
	QString form_credential_id;
	quint64 request_serial = 0;
	bool populating_form = false;
	bool preparing_layer = false;
	bool busy = false;
};

}  // namespace OpenOrienteering

#endif