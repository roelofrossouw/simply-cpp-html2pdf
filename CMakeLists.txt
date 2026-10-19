cmake_minimum_required(VERSION 3.22)
set(SC_MODULE html2pdf)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-html2pdf" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_HTML2PDF_BUILD_TESTS "Build simply-cpp-html2pdf tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.12.0)
# The HTML engine (litehtml) and the PDF writer (PDFHummus).
find_or_install_package(sc-litehtml simply-cpp-litehtml-dev simply-cpp-litehtml 1.0.1)
find_or_install_package(sc-pdf simply-cpp-pdf-dev simply-cpp-pdf 3.1.1)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(html2pdf
        LINK_LIBRARIES sc::sc-litehtml sc::sc-pdf
        PUBLIC_LINK_LIBRARIES sc::sc-litehtml sc::sc-pdf)

add_sc_libraries(DESCRIPTION "simply-cpp-html2pdf")

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-html2pdf
        COMPONENT development)

if (SC_HTML2PDF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-html2pdf-test)
endif ()

add_sc_example(sc-html2pdf-demo)

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 1.12.0~)" "simply-cpp-litehtml-dev (>= 1.0.1~)" "simply-cpp-pdf-dev (>= 3.1.1~)")
