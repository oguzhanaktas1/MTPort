# Unit + integration tests (fake MTP device; no hardware, no macFUSE required).
add_executable(mtport-tests
  test_main.cpp
  unit/test_virtual_paths.cpp
  unit/test_error_mapping.cpp
  unit/test_cache.cpp
  unit/test_file_handles.cpp
  unit/test_staged_writes.cpp
  unit/test_platform.cpp
  integration/test_fake_filesystem.cpp
  integration/test_agent_ipc.cpp
  integration/test_webdav.cpp)
target_include_directories(mtport-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(TARGET mtport-fuse-adapter)
  target_sources(mtport-tests PRIVATE unit/test_fuse_adapter.cpp)
  target_link_libraries(mtport-tests PRIVATE mtport-fuse-adapter)
endif()
target_link_libraries(mtport-tests PRIVATE mtport-agent-lib mtport-webdav)

function(af_suite name)
  add_test(NAME ${name} COMMAND mtport-tests ${name})
endfunction()
foreach(s virtual_paths error_mapping cache file_handles staged_writes fake_filesystem eof_regression platform agent_core agent_ipc json webdav)
  af_suite(${s})
endforeach()
if(TARGET mtport-fuse-adapter)
  af_suite(fuse_adapter)
endif()

# CLI end-to-end against the MOCK backend, and the forbidden-API guard.
add_test(NAME cli_mock_roundtrip COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_roundtrip.sh $<TARGET_FILE:mtport>)
add_test(NAME forbidden_api COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_forbidden_api.sh ${CMAKE_SOURCE_DIR})
