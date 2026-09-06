/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#ifndef OPENORIENTEERING_GDAL_OGC_T_H
#define OPENORIENTEERING_GDAL_OGC_T_H

#include <QObject>

namespace OpenOrienteering {

class GdalOgcTest final : public QObject
{
	Q_OBJECT

private slots:
	void validatesHttpUrls();
	void protectsQueryApiKeys();
	void describesWmsSources();
	void describesWmtsSources();
	void usesPackagedCaBundleOnWindows();
	void supportsRegionalOverviewZoom();
	void preservesProjectConfigurationWithoutSecrets();
};

}  // namespace OpenOrienteering

#endif