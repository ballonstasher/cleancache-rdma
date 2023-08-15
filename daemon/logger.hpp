#ifndef __LOGGER_HPP__
#define __LOGGER_HPP__

#include <iomanip>
#include <iostream>
#include <fstream>
#include <cstdarg>
#include <ctime>
#include <sstream>
#include <cstring>
#include <cstdio>

#define _LOGDIR_ "/root/dcc-log/"

enum log_config {
    LOG_LEVEL_OFF = 0,
    LOG_LEVEL_FATAL,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN ,
    LOG_LEVEL_INFO ,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_TRACE,
    LOG_LEVEL_ALL,
};

#define fatal(str, ...) \
    writeLog(__FUNCTION__, __LINE__, LOG_LEVEL_FATAL, str, __VA_ARGS__)
#define error(str, ...) \
    writeLog(__FUNCTION__, __LINE__, LOG_LEVEL_ERROR, str, __VA_ARGS__)
#define warn(str, ...) \
    writeLog(__FUNCTION__, __LINE__, LOG_LEVEL_WARN, str, __VA_ARGS__)
#define info(str, ...) \
    writeLog(__FUNCTION__, __LINE__, LOG_LEVEL_INFO, str, __VA_ARGS__)
#define debug(str, ...) \
    writeLog(__FUNCTION__, __LINE__, LOG_LEVEL_DEBUG, str, __VA_ARGS__)
#define trace(str, ...) \
    writeLog(__FUNCTION__, __LINE__, LOG_LEVEL_TRACE, str, __VA_ARGS__)

class Logger {
	private:
		int logLevel;
        bool simple_log;
		bool isOutput;
        std::string getTimestamp();
        char logfile[100] = {};
        char open_mode[1];

	public:
		Logger();
		Logger(int level, const char *_logfile, bool append);
        
        ~Logger() {
            free(logfile);    
        }

		void writeLog(const char *, int, int, const char *, ...);
};

#endif // __LOGGER_HPP__
