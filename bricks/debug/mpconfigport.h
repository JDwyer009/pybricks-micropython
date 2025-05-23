// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2023 The Pybricks Authors

#include "stm32f446xx.h"

#define MICROPY_HW_BOARD_NAME                   "NUCLEO-F446ZE"
#define MICROPY_HW_MCU_NAME                     "STM32F446ZE"

#define PYBRICKS_HUB_NAME                       "debug"
#define PYBRICKS_HUB_DEBUG                      (1)

// Pybricks modules
#define PYBRICKS_PY_COMMON                      (1)
#define PYBRICKS_PY_COMMON_BLE                  (0)
#define PYBRICKS_PY_COMMON_CHARGER              (0)
#define PYBRICKS_PY_COMMON_CONTROL              (0)
#define PYBRICKS_PY_COMMON_IMU                  (0)
#define PYBRICKS_PY_COMMON_KEYPAD               (1)
#define PYBRICKS_PY_COMMON_KEYPAD_HUB_BUTTONS   (1)
#define PYBRICKS_PY_COMMON_LIGHT_ARRAY          (0)
#define PYBRICKS_PY_COMMON_LIGHT_MATRIX         (0)
#define PYBRICKS_PY_COMMON_MOTORS               (0)
#define PYBRICKS_PY_COMMON_SPEAKER              (0)
#define PYBRICKS_PY_COMMON_SYSTEM               (0)
#define PYBRICKS_PY_EV3DEVICES                  (0)
#define PYBRICKS_PY_EXPERIMENTAL                (0)
#define PYBRICKS_PY_HUBS                        (0)
#define PYBRICKS_PY_IODEVICES                   (0)
#define PYBRICKS_PY_MEDIA                       (0)
#define PYBRICKS_PY_MEDIA_EV3DEV                (0)
#define PYBRICKS_PY_NXTDEVICES                  (0)
#define PYBRICKS_PY_PARAMETERS                  (1)
#define PYBRICKS_PY_PARAMETERS_BUTTON           (1)
#define PYBRICKS_PY_PARAMETERS_ICON             (0)
#define PYBRICKS_PY_DEVICES                     (0)
#define PYBRICKS_PY_PUPDEVICES                  (0)
#define PYBRICKS_PY_ROBOTICS                    (0)
#define PYBRICKS_PY_TOOLS                       (1)
#define PYBRICKS_PY_TOOLS_HUB_MENU              (0)

// Pybricks options
#define PYBRICKS_OPT_COMPILER                   (0)
#define PYBRICKS_OPT_FLOAT                      (0)
#define PYBRICKS_OPT_TERSE_ERR                  (1)
#define PYBRICKS_OPT_EXTRA_MOD                  (0)
#define PYBRICKS_OPT_CUSTOM_IMPORT              (1)
#define PYBRICKS_OPT_NATIVE_MOD                 (0)

#include "../_common_stm32/mpconfigport.h"
