add_library(mtport-agent-lib STATIC
  AgentCore.cpp DeviceCoordinator.cpp IpcServer.cpp MountSupervisor.cpp)
target_include_directories(mtport-agent-lib PUBLIC .)
target_link_libraries(mtport-agent-lib PUBLIC mtport-common mtport-cli-lib)

add_executable(mtport-agent AgentMain.cpp)
target_link_libraries(mtport-agent PRIVATE mtport-agent-lib)
