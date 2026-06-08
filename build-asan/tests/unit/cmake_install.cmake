# Install script for directory: /Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/tests/unit

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/opt/homebrew/opt/llvm@17/bin/llvm-objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/core/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/adt/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/mailbox/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/sched/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/mem/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/cli/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/config/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/ref/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/log/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/tracing/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/net/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/supervision/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/actor/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/spawn/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/fault/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/types/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/skg7on/Workspace/Projects/HPActor/.claude/worktrees/refactor-actor-system-v2/build-asan/tests/unit/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
