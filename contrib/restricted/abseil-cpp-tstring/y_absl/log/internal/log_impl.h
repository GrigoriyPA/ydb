// Copyright 2022 The Abseil Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef Y_ABSL_LOG_INTERNAL_LOG_IMPL_H_
#define Y_ABSL_LOG_INTERNAL_LOG_IMPL_H_

#include "y_absl/log/absl_vlog_is_on.h"
#include "y_absl/log/internal/conditions.h"
#include "y_absl/log/internal/log_message.h"
#include "y_absl/log/internal/strip.h"

// Y_ABSL_LOG()
#define Y_ABSL_LOG_INTERNAL_LOG_IMPL(severity)             \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATELESS, true) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

// Y_ABSL_PLOG()
#define Y_ABSL_LOG_INTERNAL_PLOG_IMPL(severity)              \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATELESS, true)   \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream() \
          .WithPerror()

// Y_ABSL_DLOG()
#ifndef NDEBUG
#define Y_ABSL_LOG_INTERNAL_DLOG_IMPL(severity)            \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATELESS, true) \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()
#else
#define Y_ABSL_LOG_INTERNAL_DLOG_IMPL(severity)             \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATELESS, false) \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()
#endif

// The `switch` ensures that this expansion is the beginning of a statement (as
// opposed to an expression). The use of both `case 0` and `default` is to
// suppress a compiler warning.
#define Y_ABSL_LOG_INTERNAL_VLOG_IMPL(verbose_level)                         \
  switch (const int absl_logging_internal_verbose_level = (verbose_level)) \
  case 0:                                                                  \
  default:                                                                 \
    Y_ABSL_LOG_INTERNAL_LOG_IF_IMPL(                                         \
        _INFO, Y_ABSL_VLOG_IS_ON(absl_logging_internal_verbose_level))       \
        .WithVerbosity(absl_logging_internal_verbose_level)

#ifndef NDEBUG
#define Y_ABSL_LOG_INTERNAL_DVLOG_IMPL(verbose_level)                        \
  switch (const int absl_logging_internal_verbose_level = (verbose_level)) \
  case 0:                                                                  \
  default:                                                                 \
    Y_ABSL_LOG_INTERNAL_DLOG_IF_IMPL(                                         \
        _INFO, Y_ABSL_VLOG_IS_ON(absl_logging_internal_verbose_level))       \
        .WithVerbosity(absl_logging_internal_verbose_level)
#else
#define Y_ABSL_LOG_INTERNAL_DVLOG_IMPL(verbose_level)                           \
  switch (const int absl_logging_internal_verbose_level = (verbose_level))    \
  case 0:                                                                     \
  default:                                                                    \
    Y_ABSL_LOG_INTERNAL_DLOG_IF_IMPL(                                            \
        _INFO, false && Y_ABSL_VLOG_IS_ON(absl_logging_internal_verbose_level)) \
        .WithVerbosity(absl_logging_internal_verbose_level)
#endif

#define Y_ABSL_LOG_INTERNAL_LOG_IF_IMPL(severity, condition)    \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATELESS, condition) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()
#define Y_ABSL_LOG_INTERNAL_PLOG_IF_IMPL(severity, condition)   \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATELESS, condition) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()    \
          .WithPerror()

#ifndef NDEBUG
#define Y_ABSL_LOG_INTERNAL_DLOG_IF_IMPL(severity, condition)   \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATELESS, condition) \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()
#else
#define Y_ABSL_LOG_INTERNAL_DLOG_IF_IMPL(severity, condition)              \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATELESS, false && (condition)) \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()
#endif

// Y_ABSL_LOG_EVERY_N
#define Y_ABSL_LOG_INTERNAL_LOG_EVERY_N_IMPL(severity, n)            \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, true)(EveryN, n) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

// Y_ABSL_LOG_FIRST_N
#define Y_ABSL_LOG_INTERNAL_LOG_FIRST_N_IMPL(severity, n)            \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, true)(FirstN, n) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

// Y_ABSL_LOG_EVERY_POW_2
#define Y_ABSL_LOG_INTERNAL_LOG_EVERY_POW_2_IMPL(severity)           \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, true)(EveryPow2) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

// Y_ABSL_LOG_EVERY_N_SEC
#define Y_ABSL_LOG_INTERNAL_LOG_EVERY_N_SEC_IMPL(severity, n_seconds)           \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, true)(EveryNSec, n_seconds) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_PLOG_EVERY_N_IMPL(severity, n)           \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, true)(EveryN, n) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()         \
          .WithPerror()

#define Y_ABSL_LOG_INTERNAL_PLOG_FIRST_N_IMPL(severity, n)           \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, true)(FirstN, n) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()         \
          .WithPerror()

#define Y_ABSL_LOG_INTERNAL_PLOG_EVERY_POW_2_IMPL(severity)          \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, true)(EveryPow2) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()         \
          .WithPerror()

#define Y_ABSL_LOG_INTERNAL_PLOG_EVERY_N_SEC_IMPL(severity, n_seconds)          \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, true)(EveryNSec, n_seconds) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()                    \
          .WithPerror()

#ifndef NDEBUG
#define Y_ABSL_LOG_INTERNAL_DLOG_EVERY_N_IMPL(severity, n) \
  Y_ABSL_LOG_INTERNAL_CONDITION_INFO(STATEFUL, true)       \
  (EveryN, n) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_FIRST_N_IMPL(severity, n) \
  Y_ABSL_LOG_INTERNAL_CONDITION_INFO(STATEFUL, true)       \
  (FirstN, n) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_EVERY_POW_2_IMPL(severity) \
  Y_ABSL_LOG_INTERNAL_CONDITION_INFO(STATEFUL, true)        \
  (EveryPow2) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_EVERY_N_SEC_IMPL(severity, n_seconds) \
  Y_ABSL_LOG_INTERNAL_CONDITION_INFO(STATEFUL, true)                   \
  (EveryNSec, n_seconds) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#else  // def NDEBUG
#define Y_ABSL_LOG_INTERNAL_DLOG_EVERY_N_IMPL(severity, n) \
  Y_ABSL_LOG_INTERNAL_CONDITION_INFO(STATEFUL, false)      \
  (EveryN, n) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_FIRST_N_IMPL(severity, n) \
  Y_ABSL_LOG_INTERNAL_CONDITION_INFO(STATEFUL, false)      \
  (FirstN, n) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_EVERY_POW_2_IMPL(severity) \
  Y_ABSL_LOG_INTERNAL_CONDITION_INFO(STATEFUL, false)       \
  (EveryPow2) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_EVERY_N_SEC_IMPL(severity, n_seconds) \
  Y_ABSL_LOG_INTERNAL_CONDITION_INFO(STATEFUL, false)                  \
  (EveryNSec, n_seconds) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()
#endif  // def NDEBUG

#define Y_ABSL_LOG_INTERNAL_VLOG_EVERY_N_IMPL(verbose_level, n)                \
  switch (const int absl_logging_internal_verbose_level = (verbose_level))   \
  case 0:                                                                    \
  default:                                                                   \
    Y_ABSL_LOG_INTERNAL_CONDITION_INFO(                                        \
        STATEFUL, Y_ABSL_VLOG_IS_ON(absl_logging_internal_verbose_level))      \
  (EveryN, n) Y_ABSL_LOGGING_INTERNAL_LOG_INFO.InternalStream().WithVerbosity( \
      absl_logging_internal_verbose_level)

#define Y_ABSL_LOG_INTERNAL_VLOG_FIRST_N_IMPL(verbose_level, n)                \
  switch (const int absl_logging_internal_verbose_level = (verbose_level))   \
  case 0:                                                                    \
  default:                                                                   \
    Y_ABSL_LOG_INTERNAL_CONDITION_INFO(                                        \
        STATEFUL, Y_ABSL_VLOG_IS_ON(absl_logging_internal_verbose_level))      \
  (FirstN, n) Y_ABSL_LOGGING_INTERNAL_LOG_INFO.InternalStream().WithVerbosity( \
      absl_logging_internal_verbose_level)

#define Y_ABSL_LOG_INTERNAL_VLOG_EVERY_POW_2_IMPL(verbose_level)               \
  switch (const int absl_logging_internal_verbose_level = (verbose_level))   \
  case 0:                                                                    \
  default:                                                                   \
    Y_ABSL_LOG_INTERNAL_CONDITION_INFO(                                        \
        STATEFUL, Y_ABSL_VLOG_IS_ON(absl_logging_internal_verbose_level))      \
  (EveryPow2) Y_ABSL_LOGGING_INTERNAL_LOG_INFO.InternalStream().WithVerbosity( \
      absl_logging_internal_verbose_level)

#define Y_ABSL_LOG_INTERNAL_VLOG_EVERY_N_SEC_IMPL(verbose_level, n_seconds)  \
  switch (const int absl_logging_internal_verbose_level = (verbose_level)) \
  case 0:                                                                  \
  default:                                                                 \
    Y_ABSL_LOG_INTERNAL_CONDITION_INFO(                                      \
        STATEFUL, Y_ABSL_VLOG_IS_ON(absl_logging_internal_verbose_level))    \
  (EveryNSec, n_seconds) Y_ABSL_LOGGING_INTERNAL_LOG_INFO.InternalStream()   \
      .WithVerbosity(absl_logging_internal_verbose_level)

#define Y_ABSL_LOG_INTERNAL_LOG_IF_EVERY_N_IMPL(severity, condition, n)   \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryN, n) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_LOG_IF_FIRST_N_IMPL(severity, condition, n)   \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(FirstN, n) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_LOG_IF_EVERY_POW_2_IMPL(severity, condition)  \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryPow2) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_LOG_IF_EVERY_N_SEC_IMPL(severity, condition,  \
                                                  n_seconds)            \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryNSec, \
                                                             n_seconds) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_PLOG_IF_EVERY_N_IMPL(severity, condition, n)  \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryN, n) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()              \
          .WithPerror()

#define Y_ABSL_LOG_INTERNAL_PLOG_IF_FIRST_N_IMPL(severity, condition, n)  \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(FirstN, n) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()              \
          .WithPerror()

#define Y_ABSL_LOG_INTERNAL_PLOG_IF_EVERY_POW_2_IMPL(severity, condition) \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryPow2) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()              \
          .WithPerror()

#define Y_ABSL_LOG_INTERNAL_PLOG_IF_EVERY_N_SEC_IMPL(severity, condition, \
                                                   n_seconds)           \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryNSec, \
                                                             n_seconds) \
      Y_ABSL_LOGGING_INTERNAL_LOG##severity.InternalStream()              \
          .WithPerror()

#ifndef NDEBUG
#define Y_ABSL_LOG_INTERNAL_DLOG_IF_EVERY_N_IMPL(severity, condition, n)  \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryN, n) \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_IF_FIRST_N_IMPL(severity, condition, n)  \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(FirstN, n) \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_IF_EVERY_POW_2_IMPL(severity, condition) \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryPow2) \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_IF_EVERY_N_SEC_IMPL(severity, condition, \
                                                   n_seconds)           \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, condition)(EveryNSec, \
                                                             n_seconds) \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#else  // def NDEBUG
#define Y_ABSL_LOG_INTERNAL_DLOG_IF_EVERY_N_IMPL(severity, condition, n)   \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, false && (condition))( \
      EveryN, n) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_IF_FIRST_N_IMPL(severity, condition, n)   \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, false && (condition))( \
      FirstN, n) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_IF_EVERY_POW_2_IMPL(severity, condition)  \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, false && (condition))( \
      EveryPow2) Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()

#define Y_ABSL_LOG_INTERNAL_DLOG_IF_EVERY_N_SEC_IMPL(severity, condition,  \
                                                   n_seconds)            \
  Y_ABSL_LOG_INTERNAL_CONDITION##severity(STATEFUL, false && (condition))( \
      EveryNSec, n_seconds)                                              \
      Y_ABSL_LOGGING_INTERNAL_DLOG##severity.InternalStream()
#endif  // def NDEBUG

#endif  // Y_ABSL_LOG_INTERNAL_LOG_IMPL_H_
