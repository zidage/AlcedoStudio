# Alcedo CMake for the pinned QuickQanava checkout.
#
# Upstream src/CMakeLists.txt calls `qt_wrap_cpp(qan_source_files, qan_header_files)`
# (comma in the argument). Qt 6.9.3 treats that as an unknown file extension and
# stops configure. Alcedo therefore owns qt_add_qml_module for the same sources.
# Do not add_subdirectory() the upstream CMake files. Do not FetchContent.

set(_alcedo_quickqanava_src "${ALCEDO_QUICKQANAVA_SOURCE_DIR}/src")

set(_alcedo_quickqanava_cpp
    qanBehaviour.cpp
    qanBottomRightResizer.cpp
    qanRightResizer.cpp
    qanBottomResizer.cpp
    qanConnector.cpp
    qanDraggable.cpp
    qanDraggableCtrl.cpp
    qanEdge.cpp
    qanEdgeItem.cpp
    qanEdgeDraggableCtrl.cpp
    qanGraph.cpp
    qanGraphView.cpp
    qanGrid.cpp
    qanLineGrid.cpp
    qanGroup.cpp
    qanGroupItem.cpp
    qanNavigable.cpp
    qanNavigablePreview.cpp
    qanNode.cpp
    qanNodeItem.cpp
    qanPortItem.cpp
    qanSelectable.cpp
    qanStyle.cpp
    qanStyleManager.cpp
    qanAnalysisTimeHeatMap.cpp
    qanUtils.cpp
    qanTableGroup.cpp
    qanTableCell.cpp
    qanTableBorder.cpp
    qanTableGroupItem.cpp
    qanTreeLayouts.cpp
    quickcontainers/qcmContainerModel.cpp
    quickcontainers/qcmAbstractContainer.cpp
)

set(_alcedo_quickqanava_headers
    qanAbstractDraggableCtrl.h
    qanBehaviour.h
    qanBottomRightResizer.h
    qanRightResizer.h
    qanBottomResizer.h
    qanConnector.h
    qanDraggable.h
    qanDraggableCtrl.h
    qanEdge.h
    qanEdgeDraggableCtrl.h
    qanEdgeItem.h
    qanGraph.h
    qanGraphView.h
    qanGrid.h
    qanGroup.h
    qanGroupItem.h
    qanLineGrid.h
    qanNavigable.h
    qanNavigablePreview.h
    qanNode.h
    qanNodeItem.h
    qanPortItem.h
    qanSelectable.h
    qanStyle.h
    qanStyleManager.h
    qanUtils.h
    qanTableGroup.h
    qanTableCell.h
    qanTableBorder.h
    qanTableGroupItem.h
    qanTreeLayouts.h
    QuickQanava.h
    gtpo/container_adapter.h
    gtpo/edge.h
    gtpo/graph.h
    gtpo/graph.hpp
    gtpo/graph_property.h
    gtpo/node.h
    gtpo/node.hpp
    gtpo/observable.h
    gtpo/observer.h
    quickcontainers/QuickContainers.h
    quickcontainers/qcmContainerModel.h
    quickcontainers/qcmAbstractContainer.h
    quickcontainers/qcmContainer.h
    quickcontainers/qcmAdapter.h
)

set(_alcedo_quickqanava_qml
    NavigablePreview.qml
    GraphPreview.qml
    HeatMapPreview.qml
    LineGrid.qml
    Edge.qml
    EdgeTemplate.qml
    EdgeStraightPath.qml
    EdgeOrthoPath.qml
    EdgeCurvedPath.qml
    EdgeSrcArrowPath.qml
    EdgeSrcCirclePath.qml
    EdgeSrcRectPath.qml
    EdgeDstArrowPath.qml
    EdgeDstCirclePath.qml
    EdgeDstRectPath.qml
    GraphView.qml
    Node.qml
    Port.qml
    VerticalDock.qml
    HorizontalDock.qml
    Group.qml
    TableGroup.qml
    TableCell.qml
    TableBorder.qml
    RectGroupTemplate.qml
    CanvasNodeTemplate.qml
    RectNodeTemplate.qml
    RectSolidBackground.qml
    RectSolidShadowBackground.qml
    RectShadowEffect.qml
    RectSolidGlowBackground.qml
    RectGlowEffect.qml
    RectGradientBackground.qml
    RectGradientShadowBackground.qml
    RectGradientGlowBackground.qml
    SelectionItem.qml
    VisualConnector.qml
    LabelEditor.qml
    OriginCross.qml
)

set(_alcedo_quickqanava_sources)
foreach(_rel IN LISTS _alcedo_quickqanava_cpp _alcedo_quickqanava_headers)
    set(_abs "${_alcedo_quickqanava_src}/${_rel}")
    if(NOT EXISTS "${_abs}")
        message(FATAL_ERROR
            "QuickQanava pin is missing ${_rel} under ${_alcedo_quickqanava_src}. "
            "Update alcedo_studio/src/third_party/cmake/AlcedoQuickQanava.cmake when changing the pin.")
    endif()
    list(APPEND _alcedo_quickqanava_sources "${_abs}")
endforeach()

set(_alcedo_quickqanava_qml_abs)
foreach(_rel IN LISTS _alcedo_quickqanava_qml)
    set(_abs "${_alcedo_quickqanava_src}/${_rel}")
    if(NOT EXISTS "${_abs}")
        message(FATAL_ERROR
            "QuickQanava pin is missing ${_rel} under ${_alcedo_quickqanava_src}. "
            "Update alcedo_studio/src/third_party/cmake/AlcedoQuickQanava.cmake when changing the pin.")
    endif()
    set_source_files_properties("${_abs}" PROPERTIES QT_RESOURCE_ALIAS "${_rel}")
    list(APPEND _alcedo_quickqanava_qml_abs "${_abs}")
endforeach()

set(_alcedo_quickqanava_binary_dir "${CMAKE_BINARY_DIR}/third_party/QuickQanava")
file(MAKE_DIRECTORY "${_alcedo_quickqanava_binary_dir}")

qt_add_qml_module(QuickQanava
    STATIC
    URI QuickQanava
    SOURCES ${_alcedo_quickqanava_sources}
    QML_FILES ${_alcedo_quickqanava_qml_abs}
    RESOURCE_PREFIX /
    OUTPUT_DIRECTORY "${_alcedo_quickqanava_binary_dir}/QuickQanava"
)

target_include_directories(QuickQanava
    PUBLIC
        "$<BUILD_INTERFACE:${_alcedo_quickqanava_src}>"
)

target_link_libraries(QuickQanava
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
)

target_compile_options(QuickQanava PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/w>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)

if(TARGET QuickQanavaplugin)
    target_compile_options(QuickQanavaplugin PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/w>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)
endif()

unset(_alcedo_quickqanava_src)
unset(_alcedo_quickqanava_cpp)
unset(_alcedo_quickqanava_headers)
unset(_alcedo_quickqanava_qml)
unset(_alcedo_quickqanava_sources)
unset(_alcedo_quickqanava_qml_abs)
unset(_alcedo_quickqanava_binary_dir)
unset(_rel)
unset(_abs)
