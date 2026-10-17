// config.tint: with it on, a message follows the decoration in its severity's palette color;
// with it off (the default, `logger_tint_test off`), nothing is inserted there. Lines are
// formatted on the LOG thread, so each run keeps one setting. Truecolor, forced on a file sink.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>

#include "logger.h"

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		std::exit(1); \
	} \
} while (0)

int main(int argc, char** argv) {
	const bool on = !(argc > 1 && std::string(argv[1]) == "off");
	char path[] = "/tmp/logger_tint_XXXXXX";
	int fd = ::mkstemp(path);
	CHECK(fd >= 0);
	::close(fd);
	Logging::config.color = LogColorMode::always;
	Logging::config.color_depth = LogColorDepth::truecolor;
	Logging::config.log_level = LOG_DEBUG;
	Logging::handlers.clear();
	Logging::handlers.emplace_back(std::make_unique<StreamLogger>(path));

	Logging::config.tint = on;
	L_INFO("TINT_INFO");
	L_WARNING("TINT_WARNING");
	Logging::finish();

	std::ifstream in(path);
	const std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	::unlink(path);
	CHECK(out.find("TINT_INFO") != std::string::npos && out.find("TINT_WARNING") != std::string::npos);
	CHECK((out.find("38;2;63;119;179mTINT_INFO") != std::string::npos) == on);
	CHECK((out.find("38;2;255;177;66mTINT_WARNING") != std::string::npos) == on);
	std::puts("tint: ok");
	return 0;
}
