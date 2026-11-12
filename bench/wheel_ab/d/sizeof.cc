// sizeof(Logging) of the arm it is linked against, and what the allocator is asked for per record.
#include <cstdio>
#include <memory>

#include "logger.h"

int main() {
	std::printf("sizeof(Logging) %zu\n", sizeof(Logging));
	std::printf("make_shared block %zu (record + 16-byte control block)\n", sizeof(Logging) + 16);
	return 0;
}
