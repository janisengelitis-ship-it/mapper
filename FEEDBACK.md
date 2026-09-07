# WMS/WMTS + LKS-2020 Community Preview feedback guide

This file gives testers a simple structure for reporting results for **WMS/WMTS + LKS-2020 Community Preview v0.2**.

Release:
https://github.com/janisengelitis-ship-it/mapper/releases/tag/wms-wmts-lks2020-community-preview-v0.2

Source branch:
https://github.com/janisengelitis-ship-it/mapper/tree/community-preview/wms-wmts-lks2020-v0.2

Testing guide:
https://github.com/janisengelitis-ship-it/mapper/blob/community-preview/wms-wmts-lks2020-v0.2/COMMUNITY_PREVIEW.md

## Minimal test report

Please include:

- Operating system:
- Preview version/build:
- WMS or WMTS service URL:
- Selected layer:
- Mapper map CRS:
- Was LKS-2020 / EPSG:10306 used? yes/no:
- Service CRS / tile matrix set:
- Expected result:
- Actual result:
- Screenshot, if positioning, scaling or rendering is wrong:

## Useful positive report

Positive results are useful too. A short successful report can be:

```text
OS: Windows 11 x64
Preview: WMS/WMTS + LKS-2020 Community Preview v0.2
Service type: WMS or WMTS
Service URL: ...
Layer: ...
Mapper map CRS: EPSG:10306 or other
Service CRS / tile matrix set: ...
Result: connected, rendered, panned/zoomed correctly, project reopened correctly.
LKS-2020 result: CRS preset available / transformation behaved correctly / not tested.
Notes: ...
```

## Useful failure report

```text
OS: ...
Preview: ...
Service type: WMS or WMTS
Service URL: ...
Layer: ...
Mapper map CRS: ...
Was LKS-2020 / EPSG:10306 used? yes/no
Service CRS / tile matrix set: ...
Expected: ...
Actual: ...
Can it be reproduced after restarting Mapper? yes/no
Screenshot or error message: ...
```

## Especially useful test cases

- national geoportals;
- Latvian LKS-2020 / EPSG:10306 projects;
- transformation between LKS-92 and LKS-2020;
- orthophotos;
- LiDAR-derived hillshade services;
- cadastral/topographic services;
- unusual CRS combinations;
- slow services;
- authenticated services;
- project save/reopen behavior.

## Notes

This is an unofficial test build, not an official OpenOrienteering release. The purpose is practical testing and technical review before deciding whether and how this should be shaped into a proper upstream proposal.
