
#include <cstdarg>



#define LOGGER(name, indent)       Logger logger(indent, name, __FILE__, __PRETTY_FUNCTION__, __LINE__)
#define LOG(name, indent, ...)     Logger::log(name, indent, __LINE__, __VA_ARGS__)

#define LOGGER_SERVER              LOGGER("srv", LoggerIndentServer::indent)
#define LOG_SERVER(...)            LOG("srv", LoggerIndentServer::indent, __VA_ARGS__)

#define LOGGER_TEST                LOGGER("tst", LoggerIndentTest::indent)
#define LOG_TEST(...)              LOG("tst", LoggerIndentTest::indent, __VA_ARGS__)



template <typename T>
struct LoggerIndent {
  static inline int indent;
};

struct LoggerIndentServer : LoggerIndent<LoggerIndentServer> { };
struct LoggerIndentTest   : LoggerIndent<LoggerIndentTest> { };



class Logger {
 public:
  Logger(int& indent, const char* name, const char* file, const char* function, const int line)
  : indent(indent), name(name), file(file), function(function), line(line) {
    fprintf(stderr, "%s %d   %*sEntering %s : %d\n", name, indent / 2, indent, "", function, line);
    fflush(stderr);
    indent += 2;
  }

  ~Logger( ) {
    indent -= 2;
    fprintf(stderr, "%s %d   %*sLeaving %s\n", name, indent / 2, indent, "", function);
    fflush(stderr);
  }

  static void log(const char* name, int indent, int line, const char* format, ...) {
    fprintf(stderr, "%s %d   %*s: %d >>   ", name, indent / 2, indent, "", line);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
  }

 private:
  int&         indent;
  const  char* name;
  const  char* file; // TODO
  const  char* function;
  const  int   line;
};

