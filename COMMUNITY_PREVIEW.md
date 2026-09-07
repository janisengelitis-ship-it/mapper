# WMS/WMTS + LKS-2020 Community Preview v0.2

This is an **unofficial experimental build of OpenOrienteering Mapper** for evaluating generic online WMS/WMTS background maps together with **LKS-2020 / Latvia TM (EPSG:10306)** support.

The goal is simple: make the implementation easy to try, collect real-world feedback from cartographers, and use that feedback to decide what should be improved before any future upstream contribution.

This preview is not an official OpenOrienteering release.

## Download

Preview release with Windows x64 installer, portable ZIP, SHA-256 checksums and GDAL driver report:

https://github.com/janisengelitis-ship-it/mapper/releases/tag/wms-wmts-lks2020-community-preview-v0.2

For test reporting, please use the structure in `FEEDBACK.md`:

https://github.com/janisengelitis-ship-it/mapper/blob/community-preview/wms-wmts-lks2020-v0.2/FEEDBACK.md

## What is included

- Saved WMS/WMTS connection definitions.
- Automatic WMS/WMTS service and layer discovery through GDAL.
- Viewport-based online raster rendering without blocking the map paint path.
- Reprojection from the service CRS to the current Mapper map CRS when required.
- Memory/disk caching and request retry/timeout handling.
- Project save/reopen support for online raster backgrounds.
- Basic, Bearer-token and API-key authentication support.
- Secure credential persistence through Windows Credential Manager when available.
- HTTPS/TLS support in the standalone Windows package.
- **LKS-2020 / Latvia TM (EPSG:10306)** CRS preset in Mapper.
- PROJ 9.6.1+ requirement for this preview build.
- Bundled Latvian LKS-92 to LKS-2020 transformation grid `lv_lgia_lks92to2020.tif` for offline transformation support.

The WMS/WMTS implementation is intentionally **provider-neutral**. There is no LVM-, NASA-, ArcGIS- or national-geoportal-specific rendering code.

## Recommended way to test

For first testing, use the portable Windows ZIP or install the preview separately from your normal Mapper installation.

1. Create or open a georeferenced map in Mapper.
2. Set the map CRS you normally use. For Latvia testing, select **LKS-2020 / Latvia TM (EPSG:10306)**.
3. Open **Templates → Add WMS/WMTS background map...**.
4. Enter a connection name.
5. Leave **Service type** on **Auto detect** unless you specifically want to force WMS or WMTS.
6. Paste the service URL.
7. Choose authentication if the service requires it; otherwise leave **None**.
8. Press **Connect**.
9. Select a layer from the discovered list and press **Add**.
10. Pan and zoom around the map and check positioning, sharpness and responsiveness.
11. Save the Mapper project, close it, reopen it and verify that the online background and CRS are restored correctly.

## Public services useful for testing

External services can change or be temporarily unavailable. Their data licences and permitted uses are determined by the service providers; users are responsible for complying with those terms.

| Service | Type | URL | Useful test |
| --- | --- | --- | --- |
| LVM GEO | WMS | `https://geoserver.lvmgeo.lv/wmsvector62531a9bfcfa4015856924e94076a179?` | Latvian projected data and practical orienteering-map workflow |
| Poland Geoportal NMT Shaded Relief | WMS | `https://mapy.geoportal.gov.pl/wss/service/PZGIK/NMT/GRID1/WMS/ShadedRelief` | National DEM hillshade service; EPSG:2180 and other advertised CRS combinations |
| NASA GIBS | WMTS | `https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/1.0.0/WMTSCapabilities.xml` | Large global WMTS catalogue and EPSG:3857 tiles |

For Latvia-specific testing, please check both the CRS preset itself and WMS/WMTS behaviour when the Mapper map CRS is set to **EPSG:10306**.

## What feedback is most useful

Please report successful tests as well as failures. For a reproducible report, include:

- operating system;
- preview version/build;
- WMS or WMTS service URL;
- selected layer;
- Mapper map CRS;
- service CRS / tile matrix set shown in the layer list;
- whether **LKS-2020 / EPSG:10306** was used;
- what you expected;
- what actually happened;
- screenshot if positioning, scaling or rendering is wrong.

Especially useful cases are national geoportals, orthophotos, LiDAR-derived hillshade, cadastral/topographic services, unusual CRS combinations, slow services, authenticated services, LKS-2020 transformation cases and projects reopened after saving.

## Current scope

This preview is intended to evaluate the WMS/WMTS implementation and the LKS-2020/EPSG:10306 integration needed for Latvian mapping workflows. It does not attempt to redefine the whole Mapper workflow.

The source branch is based directly on the current OpenOrienteering Mapper master used for this development and contains the WMS/WMTS feature plus the LKS-2020 CRS preset and packaging support.

Related upstream issue: **OpenOrienteering/mapper #84 — WMS (Web map service) support**.

## Development note

AI-assisted development tools were used during implementation and review. The code has also been exercised with automated tests and manual Windows testing against several real WMS/WMTS services. The purpose of this community preview is to add broader independent testing and technical review.

Feedback, criticism and alternative implementation suggestions are welcome.
