add_executable(StockParserTest
  StockParserTest.cpp
)

target_include_directories(StockParserTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(StockParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StockParserTest)
