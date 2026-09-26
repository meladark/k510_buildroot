cmake_minimum_required(VERSION 3.2)
project(parking C CXX)

set(CMAKE_C_FLAGS_RELEASE "-O2 -s")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -s")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-reorder")

include_directories(src)

set(SRC
    src/main.cc
    src/util.cc
    src/state.cc
    src/drm_out.cc
    src/camera.cc
    src/detector.cc
    src/spots.cc
    src/display.cc
    src/photos.cc
    src/web.cc
    src/wifi.cc
    src/text.cc
    src/canaan/v4l2.c
    src/canaan/cv2_utils.cc
    src/canaan/object_detect.cc
)

add_executable(parking ${SRC})
target_link_libraries(parking mediactl drm pthread freetype)
target_link_libraries(parking nncase.runtime nncase.rt_modules.k510)
target_link_libraries(parking opencv_core opencv_imgcodecs opencv_imgproc)

# Launcher: app switching, touch menu, Wi-Fi setup
add_executable(launcher
    src/launcher/main.cc
    src/launcher/apps.cc
    src/launcher/ui.cc
    src/launcher/ui_gallery.cc
    src/text.cc
    src/touch.cc
    src/drm_out.cc
    src/wifi.cc
    src/util.cc
    src/state.cc
)
target_link_libraries(launcher drm freetype pthread opencv_core opencv_imgproc opencv_imgcodecs)

install(TARGETS parking DESTINATION .)
install(TARGETS launcher DESTINATION /app/launcher)
install(FILES apps/10-parking.conf apps/20-camera-view.conf apps/30-object-detect.conf apps/40-face-detect.conf
        apps/README DESTINATION /app/launcher/apps)
install(FILES video_base.conf imx219_0.conf imx219_1.conf DESTINATION .)
install(DIRECTORY www/ DESTINATION www)
