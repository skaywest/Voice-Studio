#include "AudioIpc/WorkerRuntimeFileChunkSequence.h"
#ifdef NDEBUG
 #undef NDEBUG
#endif
#include <cassert>
using namespace voice_studio::ipc;

static WorkerRuntimeFileChunkHeader chunk(std::uint32_t entry, std::uint64_t offset,
                                          std::uint32_t bytes, bool final)
{
    WorkerRuntimeFileChunkHeader value{};
    value.entryIndex = entry;
    value.offsetBytes = offset;
    value.payloadBytes = bytes;
    value.flags = final ? kWorkerRuntimeFileChunkFlagFinal : 0;
    value.size = static_cast<std::uint16_t>(sizeof(value));
    return value;
}

int main()
{
    WorkerRuntimeFileChunkSequence sequence{3, 9000, 0, false};
    assert(sequence.isValid());
    assert(sequence.accept(chunk(3, 0, 8192, false)));
    assert(sequence.nextOffset == 8192 && !sequence.complete);
    assert(sequence.accept(chunk(3, 8192, 808, true)));
    assert(sequence.nextOffset == 9000 && sequence.complete);
    assert(!sequence.accept(chunk(3, 9000, 1, true)));

    WorkerRuntimeFileChunkSequence wrongEntry{2, 100, 0, false};
    assert(!wrongEntry.accept(chunk(1, 0, 50, false)));
    assert(wrongEntry.nextOffset == 0);

    WorkerRuntimeFileChunkSequence skipped{1, 100, 0, false};
    assert(!skipped.accept(chunk(1, 10, 10, false)));
    assert(skipped.nextOffset == 0);

    WorkerRuntimeFileChunkSequence overlap{1, 100, 50, false};
    assert(!overlap.accept(chunk(1, 40, 10, false)));
    assert(!overlap.accept(chunk(1, 60, 10, false)));

    WorkerRuntimeFileChunkSequence earlyFinal{0, 100, 0, false};
    assert(!earlyFinal.accept(chunk(0, 0, 50, true)));
    assert(earlyFinal.nextOffset == 0);

    WorkerRuntimeFileChunkSequence missingFinal{0, 100, 0, false};
    assert(!missingFinal.accept(chunk(0, 0, 100, false)));
    assert(missingFinal.nextOffset == 0);

    WorkerRuntimeFileChunkSequence tooLarge{0, 100, 0, false};
    assert(!tooLarge.accept(chunk(0, 0, 101, true)));

    WorkerRuntimeFileChunkSequence invalidIndex{kWorkerRuntimeManifestMaxEntries, 1, 0, false};
    assert(!invalidIndex.isValid());
    WorkerRuntimeFileChunkSequence invalidSize{0, 0, 0, false};
    assert(!invalidSize.isValid());

    auto malformed = chunk(0, 0, 100, true);
    malformed.reserved = 1;
    assert(!earlyFinal.accept(malformed));
    malformed = chunk(0, 0, 100, true);
    malformed.flags = 2;
    assert(!earlyFinal.accept(malformed));
    return 0;
}
