/* Copyright (c) 2005-2017, Stefan Eilemann <eile@equalizergraphics.com>
 *                          Daniel Nachbaur <danielnachbaur@gmail.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <pthread.h>

#include "log.h"

#include "clock.h"
#include "perThread.h"
#include "scopedMutex.h"
#include "spinLock.h"
#include "thread.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _MSC_VER
#include <process.h>
#include <io.h>
#define atoll _atoi64
#define snprintf _snprintf
#define getpid _getpid
#define isatty _isatty
#define fileno _fileno
#pragma warning(push)
#pragma warning(disable: 4996)
#else
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace lunchbox
{
static unsigned getLogTopics();
const size_t LENGTH_PID = 5;
const size_t LENGTH_THREAD = 8;
const size_t LENGTH_FILE = 29;
const size_t LENGTH_TIME = 6;

namespace
{
struct LogTable
{
    LogLevel level;
    std::string name;
};
#define LOG_TABLE_ENTRY(name)          \
    {                                  \
        LOG_##name, std::string(#name) \
    }

struct LogGlobals
{
    // clang-format off
    LogGlobals()
        : levels{LOG_TABLE_ENTRY(ERROR),
                 {LOG_ERROR, "WARN"},
                 LOG_TABLE_ENTRY(INFO),
                 LOG_TABLE_ENTRY(DEBUG),
                 LOG_TABLE_ENTRY(VERB),
                 LOG_TABLE_ENTRY(ALL)}
#ifdef NDEBUG
        , stream(&std::cout)
#else
        , stream(&std::cerr)
#endif
        , file(nullptr)
        , clock(&defaultClock)
        , colorSupported(checkColorSupport())
    {
    }
    // clang-format on

    LogTable levels[LOG_ALL];
    std::ostream* stream;
    std::ostream* file;
    Clock defaultClock;
    const Clock* clock;
    SpinLock lock; // The write lock
    PerThread<Log> log;
    bool colorSupported;

private:
    bool checkColorSupport() const
    {
        // Check environment variable first
        const char* noColor = std::getenv("NO_COLOR");
        if (noColor && strlen(noColor) > 0) {
            return false;
        }

        const char* forceColor = std::getenv("LB_LOG_COLOR");
        if (forceColor) {
            return strcmp(forceColor, "1") == 0 ||
                   strcmp(forceColor, "true") == 0 ||
                   strcmp(forceColor, "yes") == 0;
        }

#ifdef _WIN32
        // Try to enable ANSI escape sequences on Windows 10+
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == INVALID_HANDLE_VALUE) return false;

        DWORD dwMode = 0;
        if (!GetConsoleMode(hOut, &dwMode)) return false;

        dwMode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
        if (SetConsoleMode(hOut, dwMode)) {
            return isatty(fileno(stdout));
        }
        return false;
#else
        // Check if we're outputting to a terminal
        if (!isatty(fileno(stdout))) {
            return false;
        }

        // Check TERM environment variable
        const char* term = std::getenv("TERM");
        if (!term || strcmp(term, "dumb") == 0) {
            return false;
        }

        return true;
#endif
    }
};

LogGlobals& globals()
{
    static LogGlobals global;
    return global;
}

std::string getColorForLevel(LogLevel level, bool enabled)
{
    if (!enabled || !globals().colorSupported) {
        return "";
    }

    switch (level) {
        case LOG_ERROR:
            return LogColor::ERROR_COLOR;
        case LOG_WARN:
            return LogColor::DEBUG_COLOR;
        case LOG_INFO:
            return LogColor::INFO_COLOR;
        case LOG_DEBUG:
            return LogColor::DEBUG_COLOR;
        case LOG_VERB:
            return LogColor::VERB_COLOR;
        default:
            return "";
    }
}

std::string getResetColor(bool enabled)
{
    return (enabled && globals().colorSupported) ? LogColor::RESET : "";
}
}

namespace detail
{
/** @internal The string buffer used for logging. */
class Log : public std::streambuf
{
public:
    explicit Log(std::ostream& stream)
        : _indent(0)
        , _blocked(0)
        , _noHeader(0)
        , _newLine(true)
        , _stream(stream)
        , _colorEnabled(true)
        , _currentLogLevel(LOG_INFO)
    {
        _file[0] = 0;
        setThreadName("Unknown");
    }

    virtual ~Log() {}
    void indent() { ++_indent; }
    void exdent() { --_indent; }
    void disableFlush()
    {
        ++_blocked;
        assert(_blocked < 100);
    }
    void enableFlush()
    {
        assert(_blocked); // Too many enableFlush on log stream
        --_blocked;
    }

    void disableHeader() { ++_noHeader; } // use counted variable to allow
    void enableHeader() { --_noHeader; }  //   nested enable/disable calls

    void enableColor() { _colorEnabled = true; }
    void disableColor() { _colorEnabled = false; }
    bool isColorEnabled() const { return _colorEnabled; }
    void setCurrentLogLevel(LogLevel level) { _currentLogLevel = level; }

    void setThreadName(const std::string& name)
    {
        LBASSERT(!name.empty());
        _thread = name.substr(0, LENGTH_THREAD);
    }

    const std::string& getThreadName() const { return _thread; }
    void setLogInfo(const char* f, const int line)
    {
        LBASSERT(f);
        std::string file(f);
        const size_t length = file.length();

        if (length > LENGTH_FILE)
            file = file.substr(length - LENGTH_FILE, length);

        snprintf(_file, LENGTH_FILE + 6, "%29s:%-4d", file.c_str(), line);
    }

protected:
    int_type overflow(Log::int_type c) override
    {
        if (c == EOF)
            return EOF;

        if (_newLine)
        {
            // Add color prefix at the beginning of new lines
            std::string colorPrefix = getColorForLevel(_currentLogLevel, _colorEnabled);
            if (!colorPrefix.empty()) {
                _stringStream << colorPrefix;
            }

            if (!_noHeader)
            {
                if (lunchbox::Log::level > LOG_INFO)
                    _stringStream << std::right << std::setw(LENGTH_PID)
                                  << getpid() << "." << std::left
                                  << std::setw(LENGTH_THREAD) << _thread << " "
                                  << _file << " " << std::right
                                  << std::setw(LENGTH_TIME)
                                  << globals().clock->getTime64() << " ";
                else
                    _stringStream << std::right << std::setw(LENGTH_TIME)
                                  << globals().clock->getTime64() << " ";
            }

            for (int i = 0; i < _indent; ++i)
                _stringStream << "    ";
            _newLine = false;
        }

        _stringStream << (char)c;
        return c;
    }

    int sync() override
    {
        if (!_blocked)
        {
            std::string string = _stringStream.str();

            // Add color reset at the end if we have colors enabled
            std::string colorReset = getResetColor(_colorEnabled);
            if (!colorReset.empty() && !string.empty()) {
                // Check if the string ends with a newline
                if (string.back() == '\n') {
                    // Insert reset before the newline
                    string.insert(string.length() - 1, colorReset);
                } else {
                    // Append reset at the end
                    string += colorReset;
                }
            }

            {
                ScopedFastWrite mutex(globals().lock);
                _stream.write(string.c_str(), string.length());
                _stream.rdbuf()->pubsync();
            }
            _stringStream.str("");
        }
        _newLine = true;
        return 0;
    }

private:
    Log(const Log&);
    Log& operator=(const Log&);

    /** Short thread name. */
    std::string _thread;

    /** The current file logging. */
    char _file[35];

    /** The current indentation level. */
    int _indent;

    /** Flush reference counter. */
    int _blocked;

    /** The header disable counter. */
    int _noHeader;

    /** The flag that a new line has started. */
    bool _newLine;

    /** Color enable flag. */
    bool _colorEnabled;

    /** Current log level for coloring. */
    LogLevel _currentLogLevel;

    /** The temporary buffer. */
    std::ostringstream _stringStream;

    /** The wrapped ostream. */
    std::ostream& _stream;
};
}

int Log::level = Log::getLogLevel(getenv("LB_LOG_LEVEL"));
unsigned Log::topics = getLogTopics();
bool Log::colorEnabled = true;

Log::Log()
    : std::ostream(new detail::Log(getOutput()))
    , impl_(dynamic_cast<detail::Log*>(rdbuf()))
{
}

Log::~Log()
{
    impl_->pubsync();
    delete impl_;
}

void Log::indent()
{
    impl_->indent();
}

void Log::exdent()
{
    impl_->exdent();
}

void Log::disableFlush()
{
    impl_->disableFlush();
}

void Log::enableFlush()
{
    impl_->enableFlush();
}

void Log::forceFlush()
{
    impl_->pubsync();
}

void Log::disableHeader()
{
    impl_->disableHeader();
}

void Log::enableHeader()
{
    impl_->enableHeader();
}

void Log::enableColor()
{
    impl_->enableColor();
}

void Log::disableColor()
{
    impl_->disableColor();
}

bool Log::isColorEnabled() const
{
    return impl_->isColorEnabled();
}

void Log::setCurrentLogLevel(LogLevel level)
{
    impl_->setCurrentLogLevel(level);
}

void Log::setLogInfo(const char* file, const int line)
{
    impl_->setLogInfo(file, line);
}

void Log::setThreadName(const std::string& name)
{
    impl_->setThreadName(name);
}

const std::string& Log::getThreadName() const
{
    return impl_->getThreadName();
}

int Log::getLogLevel(const char* text)
{
    if (text)
    {
        const int num = atoi(text);
        if (num > 0 && num <= LOG_ALL)
            return num;

        for (uint32_t i = 0; i < LOG_ALL; ++i)
            if (globals().levels[i].name == text)
                return globals().levels[i].level;
    }

#ifdef NDEBUG
    return LOG_INFO;
#else
    return LOG_DEBUG;
#endif
}

std::string& Log::getLogLevelString()
{
    for (uint32_t i = 0; i < LOG_ALL; ++i)
        if (globals().levels[i].level == level)
            return globals().levels[i].name;

    return globals().levels[0].name;
}

void Log::setColorEnabled(bool enabled)
{
    colorEnabled = enabled;
}

bool Log::supportsColor()
{
    return globals().colorSupported;
}

unsigned getLogTopics()
{
    Log::level = Log::getLogLevel(getenv("LB_LOG_LEVEL"));
    const char* env = getenv("LB_LOG_TOPICS");

    if (env)
        return atoll(env);

    if (Log::level == LOG_ALL)
        return LOG_ANY;

#ifdef NDEBUG
    return 0;
#else
    return LOG_BUG;
#endif
}

Log& Log::instance()
{
    Log* log = globals().log.get();
    if (!log)
    {
        log = new Log();
        globals().log = log;
        static bool first = true;
        if (first && lunchbox::Log::level > LOG_INFO)
        {
            first = false;
            log->disableHeader();
            log->disableFlush();
            *log << std::setw(LENGTH_PID) << std::right << "PID"
                 << "." << std::setw(LENGTH_THREAD) << std::left << "Thread "
                 << "|" << std::setw(LENGTH_FILE + 5) << " Filename:line "
                 << "|" << std::right << std::setw(LENGTH_TIME) << " ms "
                 << "|"
                 << " Message" << std::endl;
            log->enableFlush();
            log->enableHeader();
        }
    }

    return *log;
}

Log& Log::instance(const char* file, const int line)
{
    Log& log = instance();
    log.setLogInfo(file, line);
    return log;
}

Log& Log::instance(const char* file, const int line, LogLevel logLevel)
{
    Log& log = instance();
    log.setLogInfo(file, line);
    log.setCurrentLogLevel(logLevel);
    return log;
}

void Log::exit()
{
    Log* log = globals().log.get();
    globals().log = nullptr;
    delete log;
}

void Log::reset()
{
    exit();

    delete globals().file;
    globals().file = nullptr;

#ifdef NDEBUG
    globals().stream = &std::cout;
#else
    globals().stream = &std::cerr;
#endif
}

void Log::setOutput(std::ostream& stream)
{
    globals().stream = &stream;
    exit();
}

bool Log::setOutput(const std::string& file)
{
    std::ofstream* newLog = new std::ofstream(file.c_str());

    if (!newLog->is_open())
    {
        LBERROR << "Can't open log file " << file << ": " << sysError
                << std::endl;
        delete newLog;
        return false;
    }

    LBDEBUG << "Redirect log to " << file << std::endl;
    setOutput(*newLog);

    delete globals().file;
    globals().file = newLog;
    return true;
}

void Log::setClock(Clock* clock)
{
    if (clock)
        globals().clock = clock;
    else
        globals().clock = &globals().defaultClock;
}

const Clock& Log::getClock()
{
    return *globals().clock;
}

std::ostream& Log::getOutput()
{
    return *globals().stream;
}

std::ostream& indent(std::ostream& os)
{
    Log* log = dynamic_cast<Log*>(&os);
    if (log)
        log->indent();
    return os;
}
std::ostream& exdent(std::ostream& os)
{
    Log* log = dynamic_cast<Log*>(&os);
    if (log)
        log->exdent();
    return os;
}

std::ostream& disableFlush(std::ostream& os)
{
    Log* log = dynamic_cast<Log*>(&os);
    if (log)
        log->disableFlush();
    return os;
}
std::ostream& enableFlush(std::ostream& os)
{
    Log* log = dynamic_cast<Log*>(&os);
    if (log)
        log->enableFlush();
    return os;
}
std::ostream& forceFlush(std::ostream& os)
{
    Log* log = dynamic_cast<Log*>(&os);
    if (log)
        log->forceFlush();
    return os;
}

std::ostream& disableHeader(std::ostream& os)
{
    Log* log = dynamic_cast<Log*>(&os);
    if (log)
        log->disableHeader();
    return os;
}
std::ostream& enableHeader(std::ostream& os)
{
    Log* log = dynamic_cast<Log*>(&os);
    if (log)
        log->enableHeader();
    return os;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}