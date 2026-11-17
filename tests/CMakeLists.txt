## TESTS ##
# What every test in this directory links against.
set(SC_TEST_LINK_LIBRARIES sc-html2pdf)

add_sc_test(html2pdf LABELS unit)
target_compile_definitions(test-html2pdf PRIVATE SC_HTML2PDF_TEST_BANNER="${CMAKE_CURRENT_SOURCE_DIR}/resource/banner.png")

# The shared library exports none of the code of the modules it links (helpers 33: it links their shared
# libraries; up to 1.6.x it carried copies of them).
add_test(NAME test-html2pdf-exports COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/exports.sh" $<TARGET_FILE:sc-html2pdf-shared>)
set_tests_properties(test-html2pdf-exports PROPERTIES LABELS unit)
