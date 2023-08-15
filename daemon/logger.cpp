#include <cassert>

#include "logger.hpp"

using namespace std;

Logger::Logger() {
	logLevel = LOG_LEVEL_ERROR;
}

Logger::Logger(int level, const char *_logfile, bool append) {
	logLevel = level;
    simple_log = true; // XXX: for simple logging
    strncpy(logfile, _logfile, strlen(_logfile));
    
    //printf("strlen(_logfile): %lu, strlen(logfile): %lu\n", 
      //      strlen(_logfile), strlen(logfile));

	FILE *fp = NULL;
    if (append)
        strcpy(open_mode, "a");
    else
        strcpy(open_mode, "w");
    
    fp = fopen(logfile, open_mode);

	if (!fp) {
		fprintf(stderr, "%s fail to open file pointer: %s\n", __func__, logfile);
        assert(0);
	}
   
	if (fp)
		fclose(fp);

    info("Open Log: %s\n",getTimestamp().c_str());
}

string Logger::getTimestamp() {
	string result;
	time_t currentSec = time(NULL);
	tm *t = localtime(&currentSec);
	ostringstream oss;

	switch(t->tm_mon) {
		case(0): result = "Jan"; break;
		case(1): result = "Feb"; break;
		case(2): result = "Mar"; break;
		case(3): result = "Apr"; break;
		case(4): result = "May"; break;
		case(5): result = "Jun"; break;
		case(6): result = "Jul"; break;
		case(7): result = "Aug"; break;
		case(8): result = "Sep"; break;
		case(9): result = "Oct"; break;
		case(10): result = "Nov"; break;
		case(11): result = "Dec"; break;
	}

	oss.clear();
	oss << " " << setfill('0') << setw(2) << t->tm_mday << " " << t->tm_year+1900;
	oss << " " << setfill('0') << setw(2) << t->tm_hour;
	oss << ":" << setfill('0') << setw(2) << t->tm_min;
	oss << ":" << setfill('0') << setw(2) << t->tm_sec << '\0';

	result = result + oss.str();

	return result;
}

void Logger::writeLog(const char *funcName, int line, int lv, 
        const char *str, ...) {
	FILE *fp = NULL;
    fp = fopen(logfile, open_mode);

    if (!fp) {
        fprintf(stderr, "%s fail to open file pointer: %s\n", __func__, logfile);
        printf("errno = %d\n", errno);
        return;
    }

    char *result = NULL;
    char level[10];
    
    if (!simple_log) {
        switch(lv) {
            case(LOG_LEVEL_FATAL): strcpy(level, "[FATAL]"); break;
            case(LOG_LEVEL_ERROR): strcpy(level, "[ERROR]"); break;
            case(LOG_LEVEL_WARN): strcpy(level, "[WARN] "); break;
            case(LOG_LEVEL_INFO): strcpy(level, "[INFO] "); break;
            case(LOG_LEVEL_DEBUG): strcpy(level, "[DEBUG]"); break;
            case(LOG_LEVEL_TRACE): strcpy(level, "[TRACE]"); break;
        }

        result = (char*)malloc(sizeof(char) * 
                (21 + strlen(funcName) + strlen(str) + 30));
        sprintf(result, "%s %s [%s:%d] : %s\n", 
                level, getTimestamp().c_str(), funcName, line, str);
    } else {
        result = (char *)malloc(sizeof(char) * strlen(str));
        sprintf(result, "%s\n", str); 
    }

    va_list args;

    va_start(args, str);
    vfprintf(fp, result, args);
    va_end(args);

    va_start(args, str);
    if (logLevel >= lv)
        vprintf(result, args); // not logged file, instead print
    va_end(args);

    if (result)
        free(result);
    if (fp)
        fclose(fp);
}
