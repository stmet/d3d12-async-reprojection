#pragma once
#include <cstdint>

struct SlotMetadata {
    uint64_t sequenceNumber;
    uint64_t fenceValue;
    uint32_t width;
    uint32_t height;
    uint32_t colorFormat;
    uint32_t depthFormat;
    uint32_t mvFormat;
    uint32_t validityFlags; // bit 0: color valid, bit 1: depth valid, bit 2: mv valid
    uint32_t padding;
};

struct SharedRingBuffer {
    uint32_t magic; // 0x52455052 (REPR in ASCII)
    uint32_t activeSlotIndex; // Optional hint, though synchronization is done via sequenceNumber and fences
    uint32_t producerPid;
    uint32_t padding;
    SlotMetadata slots[3];
};
