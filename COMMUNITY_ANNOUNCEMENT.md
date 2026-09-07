# Community announcement draft

This text is intended for posting in the OpenOrienteering Mapper WMS issue, forum, mailing list or other community channel.

---

I have prepared an unofficial community preview build for evaluating generic WMS/WMTS online background maps in OpenOrienteering Mapper.

This is not a request for immediate merge and not an official OpenOrienteering release. The intention is to make the functionality easy to try, collect real-world feedback from cartographers, and understand what should be improved before any possible upstream contribution.

Preview release with Windows x64 installer, portable ZIP and SHA-256 checksums:

https://github.com/janisengelitis-ship-it/mapper/releases/tag/wms-wmts-community-preview-v0.1

Source branch:

https://github.com/janisengelitis-ship-it/mapper/tree/community-preview/wms-wmts-v0.1

Preview notes and testing instructions:

https://github.com/janisengelitis-ship-it/mapper/blob/community-preview/wms-wmts-v0.1/COMMUNITY_PREVIEW.md

Feedback guide:

https://github.com/janisengelitis-ship-it/mapper/blob/community-preview/wms-wmts-v0.1/FEEDBACK.md

The implementation is intended to be provider-neutral. It uses GDAL for WMS/WMTS service and layer discovery, supports viewport-based raster rendering, reprojection to the current Mapper map CRS when needed, caching, timeout/retry handling, project save/reopen support, and basic/auth-token/API-key authentication options.

Feedback would be very welcome, especially on:

- whether the general approach fits Mapper’s template/background map architecture;
- successful and failed WMS/WMTS services;
- CRS and reprojection cases;
- UI wording and workflow;
- caching and project save/reopen behavior;
- what would need to change before this could become a proper upstream proposal.

Test reports with OS, service URL, selected layer, map CRS, service CRS/tile matrix set, expected result and actual result would be especially useful.

Thank you for taking a look.