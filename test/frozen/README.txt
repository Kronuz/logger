These two files are `logger.h` and `logger.cc` exactly as they were at commit 5299eb3, before the
producer path changed: the formatting into a fresh string, the wheel insert on every line, and the
shared pending counter. Never edit them.

`test/transcript.cc` is built twice, once against these files and once against the library, and
`test/transcript.cmake` requires the two outputs to be identical, byte for byte.
