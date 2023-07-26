INCLUDE(FindProtobuf)
FIND_PACKAGE(Protobuf REQUIRED)
INCLUDE_DIRECTORIES(${PROTOBUF_INCLUDE_DIR})

PROTOBUF_GENERATE_CPP(PROTO_SRC PROTO_HEADER pbrt.proto)
ADD_LIBRARY(pbrt_messages ${PROTO_HEADER} ${PROTO_SRC})

# Create a custom target to generate Protobuf files before building my_main_library
add_custom_target(pbrt_messages_target DEPENDS ${PROTO_SRC} ${PROTO_HEADER})

# Add 'my_protobuf_library' as a dependency to protobuf_target
add_dependencies(pbrt_messages_target pbrt_messages)
