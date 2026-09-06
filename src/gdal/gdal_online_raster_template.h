/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#ifndef OPENORIENTEERING_GDAL_ONLINE_RASTER_TEMPLATE_H
#define OPENORIENTEERING_GDAL_ONLINE_RASTER_TEMPLATE_H

#include <memory>

#include <QtGlobal>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>

#include "gdal/gdal_ogc_connection.h"
#include "templates/template.h"

class QPainter;
class QTimer;
class QXmlStreamReader;
class QXmlStreamWriter;

namespace OpenOrienteering {

struct GdalOnlineDatasetHolder;
struct GdalOnlinePreparedData;
using GdalOnlinePreparedDataPtr = std::shared_ptr<GdalOnlinePreparedData>;

class GdalOnlineRasterTemplate final : public Template
{
	Q_OBJECT

public:
	GdalOnlineRasterTemplate(const QString& dataset_name, Map* map);
	GdalOnlineRasterTemplate(const GdalOgcConnection& connection,
	                         const QString& dataset_name,
	                         const QString& display_name,
	                         GdalOnlinePreparedDataPtr prepared_data,
	                         Map* map);

	/** Opens and validates an online dataset without touching GUI state.
	 *
	 * This function is intentionally safe to call from a worker thread. An
	 * empty map CRS means that the service CRS will be adopted when the
	 * prepared data is installed in a template.
	 */
	static GdalOnlinePreparedDataPtr prepareDataset(
	        const GdalOgcConnection& connection,
	        const QString& dataset_name,
	        const QString& map_crs_spec,
	        QString& error);

protected:
	GdalOnlineRasterTemplate(const GdalOnlineRasterTemplate& proto);

public:
	~GdalOnlineRasterTemplate() override;

	GdalOnlineRasterTemplate* duplicate() const override;
	const char* getTemplateType() const override;
	bool isRasterGraphics() const override { return true; }
	bool canBeDrawnOnto() const override { return false; }

	bool fileExists() const override { return true; }
	LookupResult tryToFindTemplateFile(const QString& map_path) override;

	bool loadTemplateFileImpl() override;
	bool postLoadSetup(QWidget* dialog_parent, bool& out_center_in_view) override;
	void unloadTemplateFileImpl() override;

	void drawTemplate(QPainter* painter,
	                  const QRectF& clip_rect,
	                  double scale,
	                  bool on_screen,
	                  qreal opacity) const override;
	QRectF getTemplateExtent() const override;

	void saveTypeSpecificTemplateConfiguration(QXmlStreamWriter& xml) const override;
	bool loadTypeSpecificTemplateConfiguration(QXmlStreamReader& xml) override;
	bool finishTypeSpecificTemplateConfiguration() override;

public slots:
	void acceptPreparation(quint64 serial,
	                       GdalOnlinePreparedDataPtr prepared_data,
	                       const QString& error);
	void acceptRender(quint64 serial,
	                  const QRect& source_window,
	                  const QSize& requested_size,
	                  QImage image,
	                  const QString& error);

private slots:
	void mapGeoreferencingChanged();

private:
	void initializeTimers();
	bool applyPreparedData(GdalOnlinePreparedDataPtr prepared_data);
	void scheduleDatasetPreparation();
	void clearDatasetState();
	bool updateTemplateTransform();
	QRect sourceWindow(const QRectF& template_rect) const;
	QRectF templateRect(const QRect& source_window) const;
	QRect expandedWindow(const QRect& source_window) const;
	QSize requiredOutputSize(QPainter* painter,
	                         const QRectF& template_rect,
	                         bool on_screen) const;
	bool cachedImageIsAdequate(const QRect& source_window,
	                           const QSize& required_size) const;
	void drawCachedImage(QPainter* painter, const QRect& source_window) const;
	void scheduleRender(const QRect& source_window, const QSize& output_size) const;
	void startPendingRender();

	GdalOgcConnection connection;
	QString dataset_name;
	QString display_name;

	GdalOnlinePreparedDataPtr prepared_data;
	std::shared_ptr<GdalOnlineDatasetHolder> dataset;
	int raster_width = 0;
	int raster_height = 0;
	QString crs_spec;
	QTransform pixel_to_world;
	QTransform world_to_pixel;
	QRectF map_extent;

	mutable QImage cached_image;
	mutable QRect cached_source_window;
	mutable QSize cached_requested_size;
	mutable QRect pending_source_window;
	mutable QSize pending_output_size;
	QTimer* render_timer = nullptr;
	bool load_in_flight = false;
	quint64 preparation_serial = 0;
	mutable bool request_in_flight = false;
	mutable quint64 request_serial = 0;
	mutable qint64 retry_after_ms = 0;
};

}  // namespace OpenOrienteering

#endif