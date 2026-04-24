# CMake module to run protoc for protobuf code generation

function(PROTOBUF_GENERATE_CPP SRCS HDRS)
  if(NOT ARGN)
    message(SEND_ERROR "PROTOBUF_GENERATE_CPP called with no input files")
    return()
  endif()

  # Find protoc executable
  if(NOT PROTOC)
    find_program(PROTOC protoc)
    if(NOT PROTOC)
      message(SEND_ERROR "protoc not found - please install protobuf compiler")
      return()
    endif()
  endif()

  foreach(FIL ${ARGN})
    get_filename_component(FIL_abs ${FIL} ABSOLUTE)
    get_filename_component(FIL_dir ${FIL} DIRECTORY)
    get_filename_component(FIL_base ${FIL} NAME_WE)

    # Get the proto path relative to CMAKE_SOURCE_DIR/protos
    # This determines where protoc places output files
    string(REPLACE "${CMAKE_SOURCE_DIR}/protos/" "" FIL_rel "${FIL_dir}")

    # Output directory is CMAKE_CURRENT_BINARY_DIR / (proto_rel_dir)
    # e.g., if FIL is .../protos/hpactor/frame.proto, FIL_rel is "hpactor"
    # and output goes to CMAKE_CURRENT_BINARY_DIR/hpactor/
    if(FIL_rel)
      set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${FIL_rel})
    else()
      set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
    endif()

    # Ensure output directory exists
    file(MAKE_DIRECTORY ${OUT_DIR})

    set(${SRCS} ${${SRCS}} ${OUT_DIR}/${FIL_base}.pb.cc PARENT_SCOPE)
    set(${HDRS} ${${HDRS}} ${OUT_DIR}/${FIL_base}.pb.h PARENT_SCOPE)

    add_custom_command(
      OUTPUT ${OUT_DIR}/${FIL_base}.pb.cc
             ${OUT_DIR}/${FIL_base}.pb.h
      COMMAND ${PROTOC} --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
              -I${CMAKE_SOURCE_DIR}/protos
              ${FIL_abs}
      DEPENDS ${FIL}
      COMMENT "Generating ${FIL_base}.pb.[cc,h] from ${FIL}"
      VERBATIM
    )
  endforeach()
endfunction()