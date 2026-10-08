// Regression tests for decoded-message output:
//  - received UTC is stamped on every logged message (upstream #42/#43);
//  - bounded text building never overruns, even with _FORTIFY_SOURCE (#28).
#include "decode/message_log.h"
#include "util/appendf.h"
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#define REQUIRE(x) do { if (!(x)) { std::cerr << "FAILED line " << __LINE__ << ": " #x << '\n'; return 1; } } while (0)

static int messageTimes()
{
    MessageLog log; // no store: in-memory only
    const double before = (double)std::time(nullptr);
    DecodedMessage acars; acars.text = "ACARS";
    log.addAndStore(acars, 600);
    DecodedMessage su; su.text = "SU";
    log.addSuAndStore(su, 600);
    DecodedMessage plain; plain.text = "plain";
    log.add(plain);
    DecodedMessage archived; archived.timeSec = 1000.0; // already timestamped
    log.add(archived);
    const double after = (double)std::time(nullptr) + 1.0;
    auto msgs = log.snapshot();
    REQUIRE(msgs.size() == 4);
    for (int i = 0; i < 3; ++i)
        REQUIRE(msgs[i].timeSec >= before && msgs[i].timeSec <= after);
    REQUIRE(msgs[3].timeSec == 1000.0);
    return 0;
}

static int boundedAppend()
{
    // Same shape as the EGC Login ACK station table: a header plus up to 255
    // ~60-byte lines into a 1024-byte buffer. Before the fix the size argument
    // wrapped once the text passed 1024 bytes and glibc aborted.
    char buf[1024];
    int bp = 0;
    appendf(buf, sizeof(buf), bp, "LES %06X on %s ch", 0x123456u, "AOR-W");
    appendf(buf, sizeof(buf), bp, "\nStation table (%d station(s)):", 255);
    for (int i = 0; i < 255; ++i)
        appendf(buf, sizeof(buf), bp, "\n  %s LES %02d svc=[%s] down %.3f MHz",
                "AOR-W", i % 64, "EGC,Distress,Store-and-forward", 1537.705);
    REQUIRE(bp == (int)sizeof(buf) - 1);
    REQUIRE(std::strlen(buf) == sizeof(buf) - 1);
    REQUIRE(std::strncmp(buf, "LES 123456 on AOR-W ch", 22) == 0);

    // Exact fit, then further appends are no-ops that keep the terminator.
    char small[8];
    int sp = 0;
    appendf(small, sizeof(small), sp, "%s", "1234567");
    REQUIRE(sp == 7 && std::strcmp(small, "1234567") == 0);
    appendf(small, sizeof(small), sp, "%s", "more");
    REQUIRE(sp == 7 && std::strcmp(small, "1234567") == 0);

    // Truncation of a single long append.
    char tiny[5];
    int tp = 0;
    appendf(tiny, sizeof(tiny), tp, "%s", "abcdefgh");
    REQUIRE(tp == 4 && std::strcmp(tiny, "abcd") == 0);

    // A position already past the end (e.g. from older code) is clamped.
    tp = 100;
    appendf(tiny, sizeof(tiny), tp, "x");
    REQUIRE(tp == 4 && tiny[4] == 0);
    return 0;
}

int main()
{
    if (messageTimes()) return 1;
    if (boundedAppend()) return 1;
    std::cout << "PASS: received UTC stamped on ACARS/SU/log messages; bounded appends truncate without overrun\n";
    return 0;
}
