#!/usr/bin/env python3
"""Apply LKS-2020 support to the Mapper 0.9.7 source tree.

The verified Windows build runs this after the OGC source installer. The
dedicated preset keeps Mapper's existing PROJ-string storage format so that
GDAL import/export and older `.omap` files remain compatible.
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


MINIMUM_PROJ_VERSION = "9.6.1"


def run(*args: str, check: bool = True) -> str:
    process = subprocess.run(
        args,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if check and process.returncode:
        raise SystemExit(process.stdout.rstrip() or f"Command failed: {' '.join(args)}")
    return process.stdout.strip()


def repository_root() -> Path:
    raw = run("git", "rev-parse", "--show-toplevel")
    if raw.startswith("/") and shutil.which("cygpath"):
        converted = run("cygpath", "-w", raw, check=False)
        if converted:
            raw = converted
    return Path(raw)


def read_normalized(path: Path) -> tuple[str, str]:
    raw = path.read_bytes()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    return raw.decode("utf-8").replace("\r\n", "\n"), newline


def write_normalized(path: Path, text: str, newline: str) -> None:
    path.write_bytes(text.replace("\n", newline).encode("utf-8"))


def replace_once(path: Path, old: str, new: str) -> None:
    text, newline = read_normalized(path)
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            f"ERROR: expected one LKS-2020 patch anchor in {path}, found {count}."
        )
    write_normalized(path, text.replace(old, new, 1), newline)


def main() -> None:
    root = repository_root()

    replace_once(
        root / "CMakeLists.txt",
        "# We must not require a minimum version of PROJ via find_package\n"
        "# because PROJ config requires the major version to match exactly.",
        "# Require the modern PROJ API used by the LKS-2020 build.\n"
        "# The Windows build also verifies that proj.db contains EPSG:10306.",
    )

    replace_once(
        root / "CMakeLists.txt",
        "find_package(PROJ CONFIG)",
        f"find_package(PROJ {MINIMUM_PROJ_VERSION} CONFIG)",
    )

    replace_once(
        root / "CMakeLists.txt",
        "pkg_check_modules(PROJ4_PC IMPORTED_TARGET proj)",
        f"pkg_check_modules(PROJ4_PC {MINIMUM_PROJ_VERSION} IMPORTED_TARGET proj)",
    )

    replace_once(
        root / "CMakeLists.txt",
        "if(NOT PROJ_VERSION OR PROJ_VERSION VERSION_LESS 4.9)\n"
        "\tmessage(FATAL_ERROR \"At least PROJ 4.9 is required\")",
        f"if(NOT PROJ_VERSION OR PROJ_VERSION VERSION_LESS {MINIMUM_PROJ_VERSION})\n"
        f"\tmessage(FATAL_ERROR \"At least PROJ {MINIMUM_PROJ_VERSION} is required\")",
    )

    replace_once(
        root / "src/core/crs_template_implementation.cpp",
        "\ttemplates.reserve(5);",
        "\ttemplates.reserve(6);",
    )

    replace_once(
        root / "src/core/crs_template_implementation.cpp",
        "\t// EPSG\n"
        "\ttemp = std::make_unique<CRSTemplate>(",
        "\t// LKS-2020 / Latvia TM\n"
        "\ttemp = std::make_unique<CRSTemplate>(\n"
        "\t  QString::fromLatin1(\"EPSG:10306\"),\n"
        "\t  ::OpenOrienteering::Georeferencing::tr(\"LKS-2020 / Latvia TM\"),\n"
        "\t  ::OpenOrienteering::Georeferencing::tr(\"LKS-2020 coordinates\"),\n"
        "\t  QString::fromLatin1(\"+init=epsg:10306\"),\n"
        "\t  CRSTemplate::ParameterList {} );\n"
        "\ttemplates.push_back(std::move(temp));\n"
        "\n"
        "\t// EPSG\n"
        "\ttemp = std::make_unique<CRSTemplate>(",
    )

    replace_once(
        root / "test/georeferencing_t.cpp",
        "void GeoreferencingTest::testCRSTemplates()\n"
        "{\n"
        "\tauto epsg_template = CRSTemplateRegistry().find(QStringLiteral(\"EPSG\"));",
        "void GeoreferencingTest::testCRSTemplates()\n"
        "{\n"
        "\tauto lks2020_template = CRSTemplateRegistry().find(QStringLiteral(\"EPSG:10306\"));\n"
        "\tQVERIFY(lks2020_template);\n"
        "\tQCOMPARE(lks2020_template->parameters().size(), static_cast<std::size_t>(0));\n"
        "\tQCOMPARE(lks2020_template->specificationTemplate(), QStringLiteral(\"+init=epsg:10306\"));\n"
        "\tQCOMPARE(lks2020_template->coordinatesName(), QStringLiteral(\"LKS-2020 coordinates\"));\n"
        "\n"
        "\tGeoreferencing lks2020_georef;\n"
        "\tQVERIFY2(lks2020_georef.setProjectedCRS(\n"
        "\t        lks2020_template->id(),\n"
        "\t        lks2020_template->specificationTemplate()),\n"
        "\t        lks2020_georef.getErrorText().toLatin1());\n"
        "\tQCOMPARE(lks2020_georef.getState(), Georeferencing::Geospatial);\n"
        "\n"
        "\tauto epsg_template = CRSTemplateRegistry().find(QStringLiteral(\"EPSG\"));",
    )

    replace_once(
        root / "packaging/CMakeLists.txt",
        '  "execute_process(COMMAND \\\"${CMAKE_COMMAND}\\\" --build '
        '\\\"${CMAKE_CURRENT_BINARY_DIR}\\\" --target \\\"${basename}-translations\\\")"',
        '  "execute_process(COMMAND \\\"${CMAKE_COMMAND}\\\" --build '
        '\\\"${CMAKE_BINARY_DIR}\\\" --target \\\"${basename}-translations\\\")"',
    )

    replace_once(
        root / "packaging/CMakeLists.txt",
        "if(Mapper_PACKAGE_PROJ)\n\tif(NOT PROJ_DATA_DIR)",
        "if(Mapper_PACKAGE_PROJ)\n"
        "\tset(Mapper_LKS2020_GRID\n"
        "\t  \"${PROJECT_SOURCE_DIR}/packaging/proj-data/lv_lgia_lks92to2020.tif\")\n"
        "\tset(Mapper_LKS2020_GRID_README\n"
        "\t  \"${PROJECT_SOURCE_DIR}/packaging/proj-data/lv_lgia_README.txt\")\n"
        "\tif(NOT EXISTS \"${Mapper_LKS2020_GRID}\")\n"
        "\t\tmessage(FATAL_ERROR \"Required Latvian transformation grid is missing: ${Mapper_LKS2020_GRID}\")\n"
        "\tendif()\n"
        "\tinstall(FILES\n"
        "\t  \"${Mapper_LKS2020_GRID}\"\n"
        "\t  \"${Mapper_LKS2020_GRID_README}\"\n"
        "\t  DESTINATION \"${MAPPER_DATA_DESTINATION}/proj\")\n"
        "\n"
        "\tif(NOT PROJ_DATA_DIR)",
    )

    diff_check = run("git", "diff", "--check", check=False)
    if diff_check:
        raise SystemExit("ERROR: git diff --check failed:\n" + diff_check)

    print("LKS-2020 source patch applied.")
    print(f"Minimum PROJ version: {MINIMUM_PROJ_VERSION}")
    print("CRS preset: EPSG:10306 — LKS-2020 / Latvia TM")
    print("Grid: lv_lgia_lks92to2020.tif")


if __name__ == "__main__":
    main()
