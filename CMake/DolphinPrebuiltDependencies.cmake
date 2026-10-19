set(DOLPHIN_DEPENDENCY_PREFIX "" CACHE PATH "Installed Release dependency bundle")
if(DOLPHIN_DEPENDENCY_PREFIX)
  find_package(DolphinDependencies REQUIRED CONFIG
    PATHS "${DOLPHIN_DEPENDENCY_PREFIX}/lib/cmake/DolphinDependencies" NO_DEFAULT_PATH)
endif()
