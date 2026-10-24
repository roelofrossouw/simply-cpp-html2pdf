## TESTS ##
# What every test in this directory links against.
set(SC_TEST_LINK_LIBRARIES sc-html2pdf)

add_sc_test(html2pdf LABELS unit)
target_compile_definitions(test-html2pdf PRIVATE SC_HTML2PDF_TEST_BANNER="${CMAKE_CURRENT_SOURCE_DIR}/resource/banner.png")
