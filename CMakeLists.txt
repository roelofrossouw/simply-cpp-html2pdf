cmake_minimum_required(VERSION 3.22)
set(SC_MODULE html2pdf)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-html2pdf" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_HTML2PDF_BUILD_TESTS "Build simply-cpp-html2pdf tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

find_or_install_package(sc-core simply-cpp-dev simply-cpp 2.2.0) # 2.1.2: colours such as #900 no longer come out a hair below 0; 2.1.3: every CSS form of rgb()/hsl()
# The HTML engine (litehtml) and the PDF writer (sc-pdflite's writer, which was sc-pdf's sc::pdf).
find_or_install_package(sc-litehtml simply-cpp-litehtml-dev simply-cpp-litehtml 1.6.0) # 1.5.4: an image in a table cell sized as shown (max-height); 1.5.5: one-sided borders without hairlines
# sc::pdflite::writer (the canvas, sc-pdf's sc::pdf until it moved into sc-pdflite; raise this floor to the
# sc-pdflite release that has writer.h) and sc::pdflite::version(), for the Producer, since 4.6.0.
find_or_install_package(sc-pdflite simply-cpp-pdflite-dev simply-cpp-pdflite 4.8.0) # 4.6.3: colour channels clamped to 0 to 1; 4.7.1: revisions 2 to 6 read, "can't read" told apart from a wrong password
# web::style, for the page boxes' CSS.
find_or_install_package(sc-web simply-cpp-web-dev simply-cpp-web 1.6.0)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(html2pdf
        LINK_LIBRARIES sc::sc-litehtml sc::sc-pdflite sc::sc-web sc::sc-core
        PUBLIC_LINK_LIBRARIES sc::sc-litehtml sc::sc-pdflite sc::sc-web sc::sc-core)

# The Producer files are written with by default.
target_compile_definitions(sc-obj-html2pdf PRIVATE SC_HTML2PDF_VERSION="${SC_VERSION}")

add_sc_libraries(DESCRIPTION "simply-cpp-html2pdf")

# The shared library links the other modules' shared libraries (helpers 33). Up to 1.6.x it linked
# their static archives and so carried its own copies of sc-pdf, sc-pdflite, sc-litehtml, sc-graphics,
# sc-web, sc-qrencode and sc-core; in PHP its copy of sc-pdflite 4.6.3's reader won over libsc-pdflite
# 4.7.0's (2026-11-16). tests/exports.sh checks nothing but html2pdf's own code is exported.
# --exclude-libs stays as a safety net for any static archive that still comes in (ELF only).
if (NOT APPLE)
    target_link_options(sc-html2pdf-shared PRIVATE "LINKER:--exclude-libs,ALL")
endif ()

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-html2pdf
        COMPONENT development)

if (SC_HTML2PDF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-html2pdf-test)
endif ()

add_sc_example(sc-html2pdf-demo)

# Named, not left to dpkg-shlibdeps: sc-graphics wants an emoji font. The simply-cpp modules it links
# are added by package_sc_module(), at the versions it was built against.
package_sc_module(DEPENDS fonts-noto-color-emoji
        DEV_DEPENDS "simply-cpp-dev (>= 1.20.0~)" "simply-cpp-litehtml-dev (>= 1.6.0~)" "simply-cpp-pdflite-dev (>= 4.8.0~)")
