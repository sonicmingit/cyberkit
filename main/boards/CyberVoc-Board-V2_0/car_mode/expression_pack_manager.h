#pragma once

#include "mepack_reader.h"

#include <cstddef>
#include <cstdint>

namespace expression_packs {

enum class Source : uint8_t { Builtin, Tf };

struct UiDescriptor {
    Source source = Source::Builtin;
    char pack_id[32]{};
    char display_name[64]{};
    int version = 1;
    bool selected = false;
};

void Start(bool tf_card_mounted);
void RequestScan();
void RequestState(const char* logical_state, const char* debug_text);
void StopPlayback();
bool Select(Source source, const char* pack_id, int version);

size_t CopyUiDescriptors(UiDescriptor* output, size_t capacity);
uint32_t Revision();
bool IsScanning();
int InvalidCount();
bool IsTfSelected();
void CurrentLabel(char* output, size_t capacity);
void StatusText(char* output, size_t capacity);

}  // namespace expression_packs
