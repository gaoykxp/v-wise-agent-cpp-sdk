/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @log
 */

#ifndef CYBER_COMMON_LOG_H_
#define CYBER_COMMON_LOG_H_

#include <cstdarg>
#include <string>


#define LEFT_BRACKET "["
#define RIGHT_BRACKET "]"

#ifndef MODULE_NAME
#define MODULE_NAME apollo::cyber::binary::GetName().c_str()
#endif



#include "spdlogger.h"

// use like stream , e.g. LogWarn << "warn log: " << 1;
#define LogDebug                                        \
    cmsr::logger::log_stream({__FILE__, __LINE__, __FUNCTION__}, spdlog::level::level_enum::debug)
#define LogInfo                                         \
    cmsr::logger::log_stream({__FILE__, __LINE__, __FUNCTION__}, spdlog::level::level_enum::info)
#define LogWarn                                         \
    cmsr::logger::log_stream({__FILE__, __LINE__, __FUNCTION__}, spdlog::level::level_enum::warn)
#define LogError                                        \
    cmsr::logger::log_stream({__FILE__, __LINE__, __FUNCTION__}, spdlog::level::level_enum::err)
#define LogFatal                                        \
    cmsr::logger::log_stream({__FILE__, __LINE__, __FUNCTION__}, spdlog::level::level_enum::critical)


const int DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, FATAL = 4, NUM_SEVERITIES = 5;

#ifndef GOOGLE_PREDICT_BRANCH_NOT_TAKEN
#if 1
#define GOOGLE_PREDICT_BRANCH_NOT_TAKEN(x) (__builtin_expect(x, 0))
#else
#define GOOGLE_PREDICT_BRANCH_NOT_TAKEN(x) x
#endif
#endif

#define LOG(severity) A##severity

#define LOG_IF(severity, condition) \
  static_cast<void>(0),             \
  !(condition) ? (void) 0 : cmsr::transx::LogMessageVoidify() & LOG(severity)

#define CHECK(condition)  \
      LOG_IF(FATAL, GOOGLE_PREDICT_BRANCH_NOT_TAKEN(!(condition))) << "Check failed: " #condition " "

#define ACHECK(cond) CHECK(cond) << LEFT_BRACKET << MODULE_NAME << RIGHT_BRACKET
#define CHECK_NOTNULL(val) \
  cmsr::transx::CheckNotNull(__FILE__, __LINE__, "'" #val "' Must be non NULL", (val))

//end using spdlog -----------------------------------------------------------------------------------------------------


#define ADEBUG_MODULE(module) \
  VLOG(4) << LEFT_BRACKET << module << RIGHT_BRACKET << "[DEBUG] "
//#define ADEBUG ADEBUG_MODULE(MODULE_NAME) //htsong TODO
//#define ADEBUG ALOG_MODULE(MODULE_NAME, INFO)
//#define AINFO ALOG_MODULE(MODULE_NAME, INFO)
//#define AWARN ALOG_MODULE(MODULE_NAME, WARN)
//#define AERROR ALOG_MODULE(MODULE_NAME, ERROR)
//#define AFATAL ALOG_MODULE(MODULE_NAME, FATAL)


#ifndef ALOG_MODULE_STREAM
#define ALOG_MODULE_STREAM(log_severity) ALOG_MODULE_STREAM_##log_severity
#endif

#ifndef ALOG_MODULE
#define ALOG_MODULE(module, log_severity) \
  ALOG_MODULE_STREAM(log_severity)(module)
#endif

#define ALOG_MODULE_STREAM_INFO(module)                         \
  google::LogMessage(__FILE__, __LINE__, google::INFO).stream() \
      << LEFT_BRACKET << module << RIGHT_BRACKET

#define ALOG_MODULE_STREAM_WARN(module)                            \
  google::LogMessage(__FILE__, __LINE__, google::WARNING).stream() \
      << LEFT_BRACKET << module << RIGHT_BRACKET

#define ALOG_MODULE_STREAM_ERROR(module)                         \
  google::LogMessage(__FILE__, __LINE__, google::ERROR).stream() \
      << LEFT_BRACKET << module << RIGHT_BRACKET

#define ALOG_MODULE_STREAM_FATAL(module)                         \
  google::LogMessage(__FILE__, __LINE__, google::FATAL).stream() \
      << LEFT_BRACKET << module << RIGHT_BRACKET

#define AINFO_IF(cond) ALOG_IF(INFO, cond, MODULE_NAME)
#define AWARN_IF(cond) ALOG_IF(WARN, cond, MODULE_NAME)
#define AERROR_IF(cond) ALOG_IF(ERROR, cond, MODULE_NAME)
#define AFATAL_IF(cond) ALOG_IF(FATAL, cond, MODULE_NAME)
#define ALOG_IF(severity, cond, module) \
  !(cond) ? (void)0                     \
          : google::LogMessageVoidify() & ALOG_MODULE(module, severity)

#define ACHECK(cond) CHECK(cond) << LEFT_BRACKET << MODULE_NAME << RIGHT_BRACKET

//#define AINFO_EVERY(freq) \
//  LOG_EVERY_N(INFO, freq) << LEFT_BRACKET << MODULE_NAME << RIGHT_BRACKET
//#define AWARN_EVERY(freq) \
//  LOG_EVERY_N(WARNING, freq) << LEFT_BRACKET << MODULE_NAME << RIGHT_BRACKET
//#define AERROR_EVERY(freq) \
//  LOG_EVERY_N(ERROR, freq) << LEFT_BRACKET << MODULE_NAME << RIGHT_BRACKET

#if !defined(RETURN_IF_NULL)
#define RETURN_IF_NULL(ptr)          \
  if (ptr == nullptr) {              \
    AWARN << #ptr << " is nullptr."; \
    return;                          \
  }
#endif

#if !defined(RETURN_VAL_IF_NULL)
#define RETURN_VAL_IF_NULL(ptr, val) \
  if (ptr == nullptr) {              \
    AWARN << #ptr << " is nullptr."; \
    return val;                      \
  }
#endif

#if !defined(RETURN_IF)
#define RETURN_IF(condition)           \
  if (condition) {                     \
    AWARN << #condition << " is met."; \
    return;                            \
  }
#endif

#if !defined(RETURN_VAL_IF)
#define RETURN_VAL_IF(condition, val)  \
  if (condition) {                     \
    AWARN << #condition << " is met."; \
    return val;                        \
  }
#endif

#if !defined(_RETURN_VAL_IF_NULL2__)
#define _RETURN_VAL_IF_NULL2__
#define RETURN_VAL_IF_NULL2(ptr, val) \
  if (ptr == nullptr) {               \
    return (val);                     \
  }
#endif

#if !defined(_RETURN_VAL_IF2__)
#define _RETURN_VAL_IF2__
#define RETURN_VAL_IF2(condition, val) \
  if (condition) {                     \
    return (val);                      \
  }
#endif

#if !defined(_RETURN_IF2__)
#define _RETURN_IF2__
#define RETURN_IF2(condition) \
  if (condition) {            \
    return;                   \
  }
#endif

#endif  // CYBER_COMMON_LOG_H_
