cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME dianji)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # ---- 用户业务代码（放根 CMakeLists，不放 cmake/stm32cubemx/）----
    # 原因：cmake/stm32cubemx/ 下的文件是 CubeMX 按 .ioc 的 IP 勾选生成的，
    # 一旦用户在 CubeMX 里重新生成代码就会被覆盖；根 CMakeLists 永远不会。
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/motor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/OLED.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/usart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/oled_diag.c

    # ---- 手动补回的 HAL 驱动（.ioc 没勾，CubeMX 不会拷进来）----
    # v0.2 起因：.ioc 里没勾 USART1，CubeMX 生成工程时把
    # stm32f1xx_hal_uart.c 裁掉、把 hal_conf.h 里 HAL_UART_MODULE_ENABLED 注释掉了。
    # 解法同当年补 hal_can.c：从固件包 STM32Cube_FW_F1_V1.8.7 原样拷回，
    # 宏 + 源都写在这里（根 CMakeLists），CubeMX 重新生成也冲不掉。
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_can.c

    # ---- CAN 协议栈（v0.5 前置，从 duoji 原样移植，2026-10-19）----
    # can_bsp.c   : bxCAN 驱动（PA11/PA12 不需要配 GPIO，外设使能自动接管；
    #               中断入口 USB_LP_CAN1_RX0_IRQHandler 也在这个文件里）
    # can_proto.c : 协议层（帧头+seq+cmd+CRC8；回归测试 44 断言在
    #               D:\19780\Documents\deepseek.1\test_can_proto.py）
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_bsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/can_proto.c
)

# ---- 手动补回的 HAL 宏 ----
# 挂在 stm32cubemx 这个 INTERFACE 库上：STM32_Drivers(OBJECT) 和 dianji 都 PUBLIC link 它，
# 所以宏一次生效两处。若挂在 dianji 上，STM32_Drivers 里的 hal_uart.c/hal_can.c 收不到。
target_compile_definitions(stm32cubemx INTERFACE
    HAL_UART_MODULE_ENABLED
    HAL_CAN_MODULE_ENABLED
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
