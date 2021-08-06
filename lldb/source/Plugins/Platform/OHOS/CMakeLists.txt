add_lldb_library(lldbPluginPlatformOHOS PLUGIN
  HdcClient.cpp
  PlatformOHOS.cpp
  PlatformOHOSRemoteGDBServer.cpp

  LINK_LIBS
    lldbCore
    lldbHost
    lldbPluginPlatformLinux
    lldbPluginPlatformGDB
  LINK_COMPONENTS
    Support
  )
