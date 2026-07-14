# Fallback finder for OpenCASCADE on macOS via Homebrew
find_path(OpenCASCADE_INCLUDE_DIR Standard_Version.hxx
    PATHS
        /opt/homebrew/include/opencascade
        /usr/local/include/opencascade
        /opt/local/include/opencascade
    DOC "OpenCASCADE include directory"
)

set(OpenCASCADE_LIBS
    TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209
    TKIGES TKXSBase TKShHealing
    TKOffset TKFeat TKFillet TKBool TKMesh TKBO TKPrim
    TKHLR TKGeomBase TKBRep TKGeomAlgo TKTopAlgo
    TKG3d TKG2d TKMath TKernel
    TKService TKV3d TKOpenGl
)

foreach(lib ${OpenCASCADE_LIBS})
    find_library(OpenCASCADE_${lib}_LIBRARY
        NAMES ${lib}
        PATHS
            /opt/homebrew/lib
            /usr/local/lib
            /opt/local/lib
    )
    if(OpenCASCADE_${lib}_LIBRARY)
        list(APPEND OpenCASCADE_LIBRARIES ${OpenCASCADE_${lib}_LIBRARY})
    endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OpenCASCADE DEFAULT_MSG
    OpenCASCADE_INCLUDE_DIR OpenCASCADE_LIBRARIES
)

if(OpenCASCADE_FOUND)
    add_library(OpenCASCADE::OpenCASCADE INTERFACE IMPORTED)
    set_target_properties(OpenCASCADE::OpenCASCADE PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${OpenCASCADE_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${OpenCASCADE_LIBRARIES}"
    )
endif()
