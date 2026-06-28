/*
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "logger.h"

#include <cerrno>        // for errno, EINTR
#include <fcntl.h>       // for open, O_WRONLY, O_CREAT, O_APPEND
#include <system_error>  // for std::system_error
#include <unistd.h>      // for write, isatty, close, STDERR_FILENO


LogConfig Logging::config;
std::vector<std::unique_ptr<Logger>> Logging::handlers;


// One marker per syslog priority (LOG_EMERG=0 .. LOG_DEBUG=7), plus a blank
// "verbose" slot. The bar glyph carries the level at a glance; the trailing
// "\033[0m" resets color, and render() strips the whole escape when color is off.
static const std::string priorities[MAX_LOG_PRIORITY + 1] = {
	"\033[1;41m█\033[0m",  // LOG_EMERG   0
	"\033[1;31m▉\033[0m",  // LOG_ALERT   1
	"\033[31m▊\033[0m",    // LOG_CRIT    2
	"\033[31m▋\033[0m",    // LOG_ERR     3
	"\033[33m▌\033[0m",    // LOG_WARNING 4
	"\033[32m▍\033[0m",    // LOG_NOTICE  5
	"\033[36m▎\033[0m",    // LOG_INFO    6
	"\033[90m▏\033[0m",    // LOG_DEBUG   7
	"",                    // verbose   > 7
};


static int
validated_priority(int priority)
{
	if (priority < 0) {
		priority = -priority;
	}
	if (priority > MAX_LOG_PRIORITY) {
		priority = MAX_LOG_PRIORITY;
	}
	return priority;
}


static bool
is_tty()
{
	static const bool tty = ::isatty(STDERR_FILENO) != 0;
	return tty;
}


// write(2) loop that retries on EINTR and short writes.
static void
write_all(int fd, const char* data, size_t size)
{
	while (size > 0) {
		ssize_t n = ::write(fd, data, size);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		data += n;
		size -= static_cast<size_t>(n);
	}
}


std::string
strip_colors(std::string_view str)
{
	std::string out;
	out.reserve(str.size());
	for (size_t i = 0; i < str.size(); ++i) {
		if (str[i] == '\033' && i + 1 < str.size() && str[i + 1] == '[') {
			i += 2;
			while (i < str.size() && str[i] != 'm') {
				++i;
			}
			// the loop's ++i steps past the terminating 'm'
		} else {
			out.push_back(str[i]);
		}
	}
	return out;
}


// Build the bytes a sink writes: optional priority marker, the message,
// optional newline, with color kept or stripped per `colorize`.
static std::string
render(int priority, std::string_view str, bool with_priority, bool with_endl, bool colorize)
{
	std::string buf;
	if (with_priority) {
		buf += priorities[priority];
	}
	buf += str;
	if (with_endl) {
		buf += '\n';
	}
	return colorize ? buf : strip_colors(buf);
}


StreamLogger::StreamLogger(const char* filename)
	: fdout(::open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644))
{
	if (fdout == -1) {
		throw std::system_error(errno, std::generic_category());
	}
}

StreamLogger::~StreamLogger() noexcept
{
	if (fdout != -1) {
		::close(fdout);
	}
}

void
StreamLogger::log(int priority, std::string_view str, bool with_priority, bool with_endl)
{
	auto buf = render(priority, str, with_priority, with_endl, Logging::config.colors);
	write_all(fdout, buf.data(), buf.size());
}


void
StderrLogger::log(int priority, std::string_view str, bool with_priority, bool with_endl)
{
	auto buf = render(priority, str, with_priority, with_endl, is_tty() || Logging::config.colors);
	write_all(STDERR_FILENO, buf.data(), buf.size());
}


SysLog::SysLog(const char* ident, int option, int facility)
{
	openlog(ident, option, facility);
}

SysLog::~SysLog() noexcept
{
	closelog();
}

void
SysLog::log(int priority, std::string_view str, bool with_priority, bool /*with_endl*/)
{
	auto buf = render(priority, str, with_priority, false, Logging::config.colors);
	syslog(priority, "%s", buf.c_str());
}


void
Logging::log(int priority, std::string str, bool with_priority, bool with_endl)
{
	priority = validated_priority(priority);
	for (auto& handler : handlers) {
		handler->log(priority, str, with_priority, with_endl);
	}
}
