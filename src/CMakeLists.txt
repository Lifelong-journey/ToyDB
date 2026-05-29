add_executable(toy_db main.cpp b_plus_tree.cpp page_manager.cpp table.cpp catalog.cpp log_manager.cpp lock_manager.cpp mvcc.cpp query_utils.cpp operators/table_scan_operator.cpp operators/filter_operator.cpp operators/project_operator.cpp operators/join_operator.cpp operators/sort_operator.cpp operators/aggregate_operator.cpp operators/limit_operator.cpp operators/query_executor.cpp)
target_link_libraries(toy_db duckdb_pg_query)

add_executable(concurrency_test concurrency_test.cpp b_plus_tree.cpp page_manager.cpp table.cpp catalog.cpp log_manager.cpp lock_manager.cpp mvcc.cpp)
target_link_libraries(concurrency_test duckdb_pg_query)

add_executable(concurrency_detailed_test concurrency_detailed_test.cpp b_plus_tree.cpp page_manager.cpp table.cpp catalog.cpp log_manager.cpp lock_manager.cpp mvcc.cpp query_utils.cpp)
target_link_libraries(concurrency_detailed_test duckdb_pg_query)
